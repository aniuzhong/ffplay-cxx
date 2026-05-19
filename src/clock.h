#pragma once

#include <cmath>

constexpr double AV_NOSYNC_THRESHOLD = 10.0;

class Clock {
public:
    Clock();

    double get(int queue_serial)                          const;
    double get_at(int queue_serial, double time)          const;
    double speed()                                        const { return speed_; }
    bool   paused()                                       const { return paused_; }
    int    serial()                                       const { return serial_; }
    double pts()                                          const { return pts_; }
    double last_updated()                                 const { return last_updated_; }
    void   set_paused(bool paused)                              { paused_ = paused; }
    void   set_speed(double speed);
    void   set(double pts, int serial);
    void   set_at(double pts, int serial, double time);

private:
    double pts_          = NAN;
    double pts_drift_    = 0;
    double last_updated_ = 0;
    double speed_        = 1.0;
    int    serial_       = -1;
    bool   paused_       = false;
};

void sync_clock_to_slave(Clock *c, int c_queue_serial, Clock *slave, int slave_queue_serial);
