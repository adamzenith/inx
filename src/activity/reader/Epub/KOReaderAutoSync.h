#pragma once

/**
 * @file KOReaderAutoSync.h
 * @brief Unattended KOReader progress sync run when an EPUB is opened or closed.
 */

#include <Epub.h>

#include <cstdint>
#include <memory>

#include "ProgressMapper.h"

/**
 * Keeps the reader and the KOReader sync server in step without asking.
 *
 * Each book remembers, in its cache directory, the position it last agreed on with the server. On open, a position
 * that moved only on the server is pulled and one that moved only here is pushed; when both moved, the newer one wins
 * (server timestamp against the time the local position last changed), falling back to the furthest one when this
 * device's clock has never been set. On close, a position that moved since the last sync is pushed.
 *
 * Nothing runs unless a saved Wi-Fi network is in range: callers connect first with connectToSavedWifi() and only
 * then show anything or free heap (the TLS handshake needs ~55KB). Both syncs turn Wi-Fi off when done.
 */
namespace KOReaderAutoSync {

/** The reader's position as handed to a sync. */
struct LocalState {
  int spineIndex = 0;
  int pageNumber = 0;
  int totalPagesInSpine = 1;
  /** False when the book has no saved progress on this device yet. */
  bool hasProgress = false;
  /** UTC seconds when the position last moved, or 0 when the clock was unset at the time. */
  uint32_t changedAt = 0;
};

enum class Result { NOTHING, PUSHED, PULLED, FAILED };

/** Earliest UTC time treated as a set clock (2024-01-01). */
constexpr uint32_t MIN_WALL_CLOCK = 1704067200;

/** True when auto sync is switched on, KOReader credentials are set and at least one Wi-Fi network is saved. */
bool isEnabled();

/** Joins the strongest saved Wi-Fi network in range. Leaves Wi-Fi off and returns false when none connects. */
bool connectToSavedWifi();

/** UTC seconds now, or 0 when the clock has never been set since power was lost. */
uint32_t wallClockNow();

/**
 * Compares the local position with the server's and moves whichever is older. Call once connected.
 * @param outRemote On PULLED, the position to jump to.
 */
Result syncOnOpen(const std::shared_ptr<Epub>& epub, const LocalState& local, PagePosition& outRemote);

/** True when the local position moved since the last sync, i.e. syncOnClose() has something to push. */
bool hasUnsyncedChanges(const std::shared_ptr<Epub>& epub, const LocalState& local);

/** Pushes the local position. Call once connected. */
Result syncOnClose(const std::shared_ptr<Epub>& epub, const LocalState& local);

}  // namespace KOReaderAutoSync
