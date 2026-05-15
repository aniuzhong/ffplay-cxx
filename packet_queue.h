#pragma once

#include <condition_variable>
#include <mutex>

struct AVPacket;
struct AVFifo;

class PacketQueue {
public:
    PacketQueue();
    ~PacketQueue();

    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    int put(AVPacket *pkt);
    int put_null_packet(int stream_index);
    int get(AVPacket *pkt, int block, int *serial);
    void flush();
    void abort();
    void start();

    bool valid() const { return pkt_list_ != nullptr; }
    int serial() const { return serial_; }
    int* serial_ptr() { return &serial_; } // TODO: remove when Clock refactored
    int nb_packets() const { return nb_packets_; }
    int size() const { return size_; }
    int64_t duration() const { return duration_; }
    bool abort_request() const { return abort_request_ != 0; }

private:
    int put_private(AVPacket *pkt);

    AVFifo *pkt_list_ = nullptr;
    int nb_packets_ = 0;
    int size_ = 0;
    int64_t duration_ = 0;
    int abort_request_ = 1;
    int serial_ = 0;
    std::mutex mutex_;
    std::condition_variable cond_;
};
