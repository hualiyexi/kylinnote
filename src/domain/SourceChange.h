#pragma once

#include <QString>
#include <QPair>

namespace kl {

struct SourceChange {
    int start = 0;
    int removed = 0;
    QString inserted;
    bool isEmpty() const { return removed == 0 && inserted.isEmpty(); }
};

inline SourceChange sourceChange(const QString& before, const QString& after) {
    int start = 0;
    while (start < before.size() && start < after.size() && before.at(start) == after.at(start)) ++start;
    if (start > 0 && start < before.size() && before.at(start).isLowSurrogate()
        && before.at(start - 1).isHighSurrogate()) --start;
    int endBefore = before.size(), endAfter = after.size();
    while (endBefore > start && endAfter > start && before.at(endBefore - 1) == after.at(endAfter - 1)) {
        --endBefore;
        --endAfter;
    }
    if (endBefore > start && endBefore < before.size() && before.at(endBefore).isLowSurrogate()
        && before.at(endBefore - 1).isHighSurrogate()) {
        ++endBefore;
        ++endAfter;
    }
    return {start, endBefore - start, after.mid(start, endAfter - start)};
}

inline int sourcePositionAfter(const SourceChange& change, int position, bool afterInsertion = true) {
    if (position < change.start) return position;
    if (position > change.start + change.removed)
        return position + change.inserted.size() - change.removed;
    return change.start + (afterInsertion ? change.inserted.size() : 0);
}

inline QPair<int, int> sourceSelectionAfter(const SourceChange& change, int anchor, int position) {
    const bool selected = anchor != position;
    const int end = qMax(anchor, position);
    int nextAnchor = sourcePositionAfter(change, anchor, !selected || anchor != end);
    int nextPosition = sourcePositionAfter(change, position, !selected || position != end);
    // When a peer replaces the whole selection, collapse after its replacement.
    if ((anchor < position && nextAnchor > nextPosition)
        || (anchor > position && nextAnchor < nextPosition))
        nextAnchor = nextPosition = qMax(nextAnchor, nextPosition);
    return qMakePair(nextAnchor, nextPosition);
}

// A composing view can receive committed edits while its preedit is still local.
// Rebase disjoint replacements; an overlapping replacement remains a recoverable
// local draft rather than silently removing text from the other view.
inline bool rebaseSourceChange(SourceChange* local, const SourceChange& remote) {
    if (!local || remote.isEmpty()) return true;
    const int localEnd = local->start + local->removed;
    const int remoteEnd = remote.start + remote.removed;
    if (remoteEnd <= local->start) {
        local->start += remote.inserted.size() - remote.removed;
        return true;
    }
    if (localEnd <= remote.start) return true;
    return false;
}

inline QString applySourceChange(const QString& text, const SourceChange& change) {
    return text.left(change.start) + change.inserted + text.mid(change.start + change.removed);
}

} // namespace kl
