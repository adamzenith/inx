/**
 * @file KOReaderAutoSync.cpp
 * @brief Definitions for KOReaderAutoSync.
 */

#include "KOReaderAutoSync.h"

#include <Arduino.h>
#include <SDCardManager.h>
#include <Serialization.h>
#include <WiFi.h>
#include <esp_sntp.h>

#include <climits>
#include <ctime>
#include <string>

#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "KOReaderSyncClient.h"
#include "state/NetworkCredential.h"

namespace {

constexpr uint8_t STATE_FILE_VERSION = 1;
constexpr uint32_t WIFI_SCAN_TIMEOUT_MS = 8000;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t NTP_TIMEOUT_MS = 4000;

/** What this device and the server last agreed on for one book. */
struct SyncState {
  int16_t spineIndex = -1;
  int16_t pageNumber = -1;
  /** The server's progress string as of that sync. */
  std::string progress;
};

std::string statePath(const std::shared_ptr<Epub>& epub) { return epub->getCachePath() + "/kosync.bin"; }

bool loadState(const std::shared_ptr<Epub>& epub, SyncState& state) {
  FsFile file;
  if (!SdMan.openFileForRead("KAS", statePath(epub).c_str(), file)) {
    return false;
  }
  uint8_t version = 0;
  serialization::readPod(file, version);
  if (version != STATE_FILE_VERSION) {
    file.close();
    return false;
  }
  serialization::readPod(file, state.spineIndex);
  serialization::readPod(file, state.pageNumber);
  serialization::readString(file, state.progress);
  file.close();
  return true;
}

void saveState(const std::shared_ptr<Epub>& epub, const SyncState& state) {
  FsFile file;
  if (!SdMan.openFileForWrite("KAS", statePath(epub).c_str(), file)) {
    return;
  }
  serialization::writePod(file, STATE_FILE_VERSION);
  serialization::writePod(file, state.spineIndex);
  serialization::writePod(file, state.pageNumber);
  serialization::writeString(file, state.progress);
  file.close();
}

void wifiOff() {
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

bool joinStrongestSavedNetwork() {
  WiFi.mode(WIFI_STA);
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  WiFi.scanNetworks(true);
  const unsigned long scanStart = millis();
  int16_t found = WiFi.scanComplete();
  while (found == WIFI_SCAN_RUNNING && millis() - scanStart < WIFI_SCAN_TIMEOUT_MS) {
    delay(100);
    found = WiFi.scanComplete();
  }

  WifiCredential best;
  int32_t bestRssi = INT32_MIN;
  for (int i = 0; i < found; i++) {
    const auto* cred = WIFI_STORE.findCredential(WiFi.SSID(i).c_str());
    if (cred && WiFi.RSSI(i) > bestRssi) {
      best = *cred;
      bestRssi = WiFi.RSSI(i);
    }
  }
  WiFi.scanDelete();

  if (best.ssid.empty()) {
    Serial.printf("[%lu] [KAS] No saved Wi-Fi network in range\n", millis());
    return false;
  }

  if (best.password.empty()) {
    WiFi.begin(best.ssid.c_str());
  } else {
    WiFi.begin(best.ssid.c_str(), best.password.c_str());
  }

  const unsigned long connectStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - connectStart < WIFI_CONNECT_TIMEOUT_MS) {
    delay(100);
  }
  const bool connected = WiFi.status() == WL_CONNECTED;
  Serial.printf("[%lu] [KAS] Wi-Fi %s: %s\n", millis(), connected ? "connected" : "failed", best.ssid.c_str());
  return connected;
}

/** Sets the clock from NTP if it has never been set, so later local changes carry a usable timestamp. */
void ensureClockSet() {
  if (time(nullptr) >= KOReaderAutoSync::MIN_WALL_CLOCK) {
    return;
  }
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  const unsigned long start = millis();
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && millis() - start < NTP_TIMEOUT_MS) {
    delay(100);
  }
}

std::string documentHash(const std::shared_ptr<Epub>& epub) {
  if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
    return KOReaderDocumentId::calculateFromFilename(epub->getPath());
  }
  return KOReaderDocumentId::calculate(epub->getPath());
}

PagePosition toPagePosition(const KOReaderAutoSync::LocalState& local) {
  PagePosition pos{};
  pos.spineIndex = local.spineIndex;
  pos.pageNumber = local.pageNumber;
  pos.totalPages = local.totalPagesInSpine;
  return pos;
}

bool movedSince(const SyncState& state, const KOReaderAutoSync::LocalState& local) {
  return state.spineIndex != local.spineIndex || state.pageNumber != local.pageNumber;
}

KOReaderAutoSync::Result push(const std::shared_ptr<Epub>& epub, const std::string& hash,
                              const KOReaderAutoSync::LocalState& local) {
  const KOReaderPosition position = ProgressMapper::toKOReader(epub, toPagePosition(local));

  KOReaderProgress progress;
  progress.document = hash;
  progress.progress = position.xpath;
  progress.percentage = position.percentage;

  const auto err = KOReaderSyncClient::updateProgress(progress);
  if (err != KOReaderSyncClient::OK) {
    Serial.printf("[%lu] [KAS] Push failed: %s\n", millis(), KOReaderSyncClient::errorString(err));
    return KOReaderAutoSync::Result::FAILED;
  }

  SyncState state;
  state.spineIndex = static_cast<int16_t>(local.spineIndex);
  state.pageNumber = static_cast<int16_t>(local.pageNumber);
  state.progress = position.xpath;
  saveState(epub, state);
  Serial.printf("[%lu] [KAS] Pushed %.2f%%\n", millis(), position.percentage * 100);
  return KOReaderAutoSync::Result::PUSHED;
}

/** Decides a conflict where both sides moved since the last sync. */
bool remoteIsNewer(const std::shared_ptr<Epub>& epub, const KOReaderProgress& remote,
                   const KOReaderAutoSync::LocalState& local) {
  if (local.changedAt != 0 && remote.timestamp > 0) {
    return remote.timestamp > static_cast<int64_t>(local.changedAt);
  }
  // No trustworthy local time: keep whichever is further into the book.
  return remote.percentage > ProgressMapper::toKOReader(epub, toPagePosition(local)).percentage;
}

KOReaderAutoSync::Result runOpenSync(const std::shared_ptr<Epub>& epub, const KOReaderAutoSync::LocalState& local,
                                     PagePosition& outRemote) {
  const std::string hash = documentHash(epub);
  if (hash.empty()) {
    return KOReaderAutoSync::Result::FAILED;
  }

  KOReaderProgress remote{};
  const auto err = KOReaderSyncClient::getProgress(hash, remote);
  if (err == KOReaderSyncClient::NOT_FOUND) {
    return local.hasProgress ? push(epub, hash, local) : KOReaderAutoSync::Result::NOTHING;
  }
  if (err != KOReaderSyncClient::OK) {
    Serial.printf("[%lu] [KAS] Fetch failed: %s\n", millis(), KOReaderSyncClient::errorString(err));
    return KOReaderAutoSync::Result::FAILED;
  }

  SyncState state;
  const bool hasState = loadState(epub, state);
  const bool localMoved = hasState ? movedSince(state, local) : local.hasProgress;
  const bool remoteMoved = !hasState || remote.progress != state.progress;

  if (!remoteMoved) {
    return localMoved ? push(epub, hash, local) : KOReaderAutoSync::Result::NOTHING;
  }
  if (localMoved && !remoteIsNewer(epub, remote, local)) {
    return push(epub, hash, local);
  }

  const KOReaderPosition remotePosition = {remote.progress, remote.percentage};
  outRemote = ProgressMapper::toCrossPoint(epub, remotePosition, local.spineIndex, local.totalPagesInSpine);

  state.spineIndex = static_cast<int16_t>(outRemote.spineIndex);
  state.pageNumber = static_cast<int16_t>(outRemote.pageNumber);
  state.progress = remote.progress;
  saveState(epub, state);

  if (outRemote.spineIndex == local.spineIndex && outRemote.pageNumber == local.pageNumber) {
    return KOReaderAutoSync::Result::NOTHING;
  }
  Serial.printf("[%lu] [KAS] Pulled %.2f%% from %s\n", millis(), remote.percentage * 100, remote.device.c_str());
  return KOReaderAutoSync::Result::PULLED;
}

}  // namespace

namespace KOReaderAutoSync {

bool isEnabled() {
  if (!KOREADER_STORE.getAutoSync() || !KOREADER_STORE.hasCredentials()) {
    return false;
  }
  WIFI_STORE.loadFromFile();
  return !WIFI_STORE.getCredentials().empty();
}

bool connectToSavedWifi() {
  if (joinStrongestSavedNetwork()) {
    return true;
  }
  wifiOff();
  return false;
}

uint32_t wallClockNow() {
  const time_t now = time(nullptr);
  return now >= MIN_WALL_CLOCK ? static_cast<uint32_t>(now) : 0;
}

Result syncOnOpen(const std::shared_ptr<Epub>& epub, const LocalState& local, PagePosition& outRemote) {
  ensureClockSet();
  const Result result = runOpenSync(epub, local, outRemote);
  wifiOff();
  return result;
}

bool hasUnsyncedChanges(const std::shared_ptr<Epub>& epub, const LocalState& local) {
  SyncState state;
  return !loadState(epub, state) || movedSince(state, local);
}

Result syncOnClose(const std::shared_ptr<Epub>& epub, const LocalState& local) {
  const std::string hash = documentHash(epub);
  const Result result = hash.empty() ? Result::FAILED : push(epub, hash, local);
  wifiOff();
  return result;
}

}  // namespace KOReaderAutoSync
