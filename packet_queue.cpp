#include "packet_queue.h"

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/fifo.h>
#include <libavutil/mem.h>
}

struct MyAVPacketList {
    AVPacket *pkt;
    int serial;
};

PacketQueue::PacketQueue()
{
    pkt_list_ = av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
}

PacketQueue::~PacketQueue()
{
    if (pkt_list_) {
        MyAVPacketList pkt1;
        while (av_fifo_read(pkt_list_, &pkt1, 1) >= 0)
            av_packet_free(&pkt1.pkt);
        av_fifo_freep2(&pkt_list_);
    }
}

int PacketQueue::put_private(AVPacket *pkt)
{
    if (abort_request_)
        return -1;

    MyAVPacketList pkt1;
    pkt1.pkt = pkt;
    pkt1.serial = serial_;

    int ret = av_fifo_write(pkt_list_, &pkt1, 1);
    if (ret < 0)
        return ret;
    nb_packets_++;
    size_ += pkt1.pkt->size + sizeof(pkt1);
    duration_ += pkt1.pkt->duration;
    cond_.notify_one();
    return 0;
}

int PacketQueue::put(AVPacket *pkt)
{
    AVPacket *pkt1 = av_packet_alloc();
    if (!pkt1) {
        av_packet_unref(pkt);
        return -1;
    }
    av_packet_move_ref(pkt1, pkt);

    int ret;
    {
        std::lock_guard lock(mutex_);
        ret = put_private(pkt1);
    }

    if (ret < 0)
        av_packet_free(&pkt1);

    return ret;
}

int PacketQueue::put_null_packet(int stream_index)
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
        return -1;
    pkt->stream_index = stream_index;
    int ret = put(pkt);
    av_packet_free(&pkt);
    return ret;
}

void PacketQueue::flush()
{
    MyAVPacketList pkt1;

    std::lock_guard lock(mutex_);
    if (pkt_list_) {
        while (av_fifo_read(pkt_list_, &pkt1, 1) >= 0)
            av_packet_free(&pkt1.pkt);
    }
    nb_packets_ = 0;
    size_ = 0;
    duration_ = 0;
    serial_++;
}

void PacketQueue::abort()
{
    {
        std::lock_guard lock(mutex_);
        abort_request_ = 1;
        cond_.notify_one();
    }
}

void PacketQueue::start()
{
    std::lock_guard lock(mutex_);
    abort_request_ = 0;
    serial_++;
}

int PacketQueue::get(AVPacket *pkt, int block, int *serial)
{
    MyAVPacketList pkt1;
    int ret;

    std::unique_lock lock(mutex_);

    for (;;) {
        if (abort_request_) {
            ret = -1;
            break;
        }

        if (av_fifo_read(pkt_list_, &pkt1, 1) >= 0) {
            nb_packets_--;
            size_ -= pkt1.pkt->size + sizeof(pkt1);
            duration_ -= pkt1.pkt->duration;
            av_packet_move_ref(pkt, pkt1.pkt);
            if (serial)
                *serial = pkt1.serial;
            av_packet_free(&pkt1.pkt);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            cond_.wait(lock);
        }
    }
    return ret;
}
