#pragma once

enum class AVSyncType {
    AudioMaster,    // default choice
    VideoMaster,
    ExternalClock,  // synchronize to an external clock
};
