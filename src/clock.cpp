#include "clock.h"

#include <chrono>

Clock::Clock()
{
    set(NAN, -1);
}

double Clock::get(int queue_serial) const
{
    return get_at(queue_serial, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

double Clock::get_at(int queue_serial, double time) const
{
    if (queue_serial != serial_)
        return NAN;
    if (paused_)
        return pts_;
    return pts_drift_ + time - (time - last_updated_) * (1.0 - speed_);
}

void Clock::set(double pts, int serial)
{
    double time = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    set_at(pts, serial, time);
}

void Clock::set_at(double pts, int serial, double time)
{
    pts_ = pts;
    last_updated_ = time;
    pts_drift_ = pts_ - time;
    serial_ = serial;
}

void Clock::set_speed(double speed)
{
    set(get(serial_), serial_);
    speed_ = speed;
}

void sync_clock_to_slave(Clock *c, int c_queue_serial, Clock *slave, int slave_queue_serial)
{
    double clock = c->get(c_queue_serial);
    double slave_clock = slave->get(slave_queue_serial);
    if (!std::isnan(slave_clock) && (std::isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        c->set(slave_clock, slave->serial());
}
