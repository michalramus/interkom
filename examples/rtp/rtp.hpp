// Minimal RTP transport for uncompressed audio, tuned for lowest latency.
//
// Wire format: RFC 3550 header + RFC 3551 L16 payload (big-endian signed
// 16-bit), mono 48 kHz, 10 ms per packet. No codec, so no algorithmic delay --
// 1.5 Mbit/s mono, which is free on a LAN and the wrong choice over a WAN.
//
// Both directions keep the audio callback free of system calls. The capture
// callback hands samples to Sender, which only writes them into a lock-free
// queue and signals a semaphore; a sender thread does the ::send(). The render
// callback calls Receiver::pull(), which only reads from a jitter buffer; a
// receive thread sits blocked in ::recv(). Each handoff costs one thread
// wakeup, tens of microseconds against a 10 ms packet.
//
// Pacing still comes from the audio device clock, not a timer: the sender
// thread transmits when the callback gives it a packet's worth and sleeps
// otherwise, so there is no second clock to drift against.

#ifndef INTERKOM_EXAMPLES_RTP_HPP
#define INTERKOM_EXAMPLES_RTP_HPP

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <random>
#include <semaphore>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace rtp {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kChannels = 1;
constexpr std::uint32_t kPtimeMs = 10;
constexpr std::uint32_t kFramesPerPacket = kSampleRate / 1000 * kPtimeMs; // 480
constexpr std::uint16_t kPort = 5004;

// L16/48000/1 has no static payload type (PT 11 is 44.1 kHz), so it needs a
// dynamic one plus an out-of-band rtpmap. See examples/rtp/l16.sdp.
constexpr std::uint8_t kPayloadType = 96;

constexpr std::size_t kHeaderBytes = 12;
constexpr std::size_t kPayloadBytes = kFramesPerPacket * sizeof(std::int16_t); // 960
constexpr std::size_t kPacketBytes = kHeaderBytes + kPayloadBytes;

// DSCP Expedited Forwarding: tells switches and routers this traffic is
// latency critical and should jump the queue.
constexpr int kDscpExpeditedForwarding = 0xB8;

// How much the send queue holds. Only ever one packet deep in practice -- the
// rest is headroom for a scheduling hiccup on the sender thread.
constexpr std::uint32_t kSendQueuePackets = 8;

// A gap longer than this is a stream restart, not packet loss; resync instead
// of concealing 100 ms of silence.
constexpr std::uint32_t kMaxConcealPackets = 8;

// How much the receive queue holds. Far more than the latency target on
// purpose: pull() trims the depth back down on every callback, so the only job
// left for capacity is absorbing a burst that lands between two callbacks.
// Overflowing it loses audio outright, which trimming does not.
constexpr std::uint32_t kReceiveQueuePackets = 50;

// How long a worker blocks before rechecking whether it was asked to stop.
constexpr std::chrono::milliseconds kStopPollInterval{100};

// Largest render callback Receiver::pull() converts in one pass; bigger
// requests are split, so this only bounds a preallocated scratch buffer.
constexpr std::size_t kMaxPullFrames = 4096;

namespace detail {

inline void write_u16(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 8);
    out[1] = static_cast<std::uint8_t>(value);
}

inline void write_u32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

inline std::uint16_t read_u16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[0]) << 8 | in[1]);
}

inline std::int16_t float_to_pcm(float sample) {
    const float clamped = std::clamp(sample, -1.0F, 1.0F);
    return static_cast<std::int16_t>(clamped * 32767.0F);
}

inline std::uint32_t random_u32() {
    std::random_device device;
    return static_cast<std::uint32_t>(device());
}

// Single-producer single-consumer sample queue. One audio thread and one
// network thread per instance, so neither side needs a lock.
class SampleRing {
  public:
    explicit SampleRing(std::size_t capacity) : buffer_(capacity + 1) {}

    std::size_t capacity() const {
        return buffer_.size() - 1;
    }

    std::size_t size() const {
        const std::size_t write = write_.load(std::memory_order_acquire);
        const std::size_t read = read_.load(std::memory_order_acquire);
        return (write >= read) ? (write - read) : (buffer_.size() - read + write);
    }

    // Producer side. Returns false without writing anything if it would not fit.
    bool push(const std::int16_t* samples, std::size_t count) {
        if (capacity() - size() < count) {
            return false;
        }
        std::size_t write = write_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < count; ++i) {
            buffer_[write] = samples[i];
            write = (write + 1 == buffer_.size()) ? 0 : write + 1;
        }
        write_.store(write, std::memory_order_release);
        return true;
    }

    // Producer side: concealment for packets that never arrived.
    bool push_silence(std::size_t count) {
        if (capacity() - size() < count) {
            return false;
        }
        std::size_t write = write_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < count; ++i) {
            buffer_[write] = 0;
            write = (write + 1 == buffer_.size()) ? 0 : write + 1;
        }
        write_.store(write, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns how many samples it actually produced.
    std::size_t pop(std::int16_t* out, std::size_t count) {
        const std::size_t available = std::min(count, size());
        std::size_t read = read_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < available; ++i) {
            out[i] = buffer_[read];
            read = (read + 1 == buffer_.size()) ? 0 : read + 1;
        }
        read_.store(read, std::memory_order_release);
        return available;
    }

    // Consumer side: throw away the oldest samples to claw back latency.
    void discard(std::size_t count) {
        const std::size_t available = std::min(count, size());
        const std::size_t read = read_.load(std::memory_order_relaxed);
        read_.store((read + available) % buffer_.size(), std::memory_order_release);
    }

  private:
    std::vector<std::int16_t> buffer_;
    std::atomic<std::size_t> read_{0};
    std::atomic<std::size_t> write_{0};
};

} // namespace detail

// Packetises audio and sends it from its own thread.
//
// send() is what the audio capture callback calls. It allocates nothing, takes
// no locks and issues no system call of its own -- it converts, queues, and
// signals the sender thread, which owns the socket.
class Sender {
  public:
    Sender(const char* host, std::uint16_t port = kPort)
        : ring_(kSendQueuePackets * kFramesPerPacket), ssrc_(detail::random_u32()),
          sequence_(static_cast<std::uint16_t>(detail::random_u32())),
          timestamp_(detail::random_u32()) {
        socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_ < 0) {
            error_ = "socket";
            return;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (::inet_pton(AF_INET, host, &address.sin_addr) != 1) {
            error_ = "inet_pton";
            return;
        }

        // Connecting a UDP socket pins the route once instead of resolving it
        // per packet, and lets us use ::send() rather than ::sendto().
        if (::connect(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
            error_ = "connect";
            return;
        }

        const int tos = kDscpExpeditedForwarding;
        ::setsockopt(socket_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

        // Non-blocking so a full socket buffer drops a packet instead of
        // stalling the sender thread. Dropping late audio is the right choice.
        const int flags = ::fcntl(socket_, F_GETFL, 0);
        if (flags < 0 || ::fcntl(socket_, F_SETFL, flags | O_NONBLOCK) < 0) {
            error_ = "fcntl";
        }
    }

    ~Sender() {
        stop();
        if (socket_ >= 0) {
            ::close(socket_);
        }
    }

    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    bool ok() const {
        return error_ == nullptr;
    }

    const char* error() const {
        return error_;
    }

    void start() {
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { send_loop(); });
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        ready_.release(); // wake the thread out of its wait
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // Called from the audio capture callback.
    void send(const std::int16_t* samples, std::size_t frames) {
        queue(samples, frames);
    }

    // Called from the audio capture callback.
    void send(const float* samples, std::size_t frames) {
        std::array<std::int16_t, kFramesPerPacket> pcm{};
        while (frames > 0) {
            const std::size_t chunk = std::min(frames, pcm.size());
            for (std::size_t i = 0; i < chunk; ++i) {
                pcm[i] = detail::float_to_pcm(samples[i]);
            }
            queue(pcm.data(), chunk);
            samples += chunk;
            frames -= chunk;
        }
    }

    std::uint64_t packets_sent() const {
        return sent_.load(std::memory_order_relaxed);
    }

    std::uint64_t send_errors() const {
        return errors_.load(std::memory_order_relaxed);
    }

    std::uint64_t frames_dropped() const {
        return dropped_.load(std::memory_order_relaxed);
    }

  private:
    void queue(const std::int16_t* samples, std::size_t frames) {
        if (!ring_.push(samples, frames)) {
            dropped_.fetch_add(frames, std::memory_order_relaxed);
            return;
        }
        if (ring_.size() >= kFramesPerPacket) {
            ready_.release();
        }
    }

    void send_loop() {
        std::array<std::int16_t, kFramesPerPacket> frame{};
        while (running_.load(std::memory_order_acquire)) {
            if (!ready_.try_acquire_for(kStopPollInterval)) {
                continue;
            }
            // Whole packets only; a partial tail waits for the next callback,
            // so the delay it adds is always under one packet.
            while (ring_.size() >= frame.size()) {
                ring_.pop(frame.data(), frame.size());
                transmit(frame.data());
            }
        }
    }

    void transmit(const std::int16_t* frame) {
        packet_[0] = 0x80; // version 2, no padding, no extension, no CSRCs
        packet_[1] = static_cast<std::uint8_t>(kPayloadType | (first_ ? 0x80 : 0x00));
        detail::write_u16(&packet_[2], sequence_);
        detail::write_u32(&packet_[4], timestamp_);
        detail::write_u32(&packet_[8], ssrc_);
        for (std::size_t i = 0; i < kFramesPerPacket; ++i) {
            detail::write_u16(&packet_[kHeaderBytes + i * sizeof(std::int16_t)],
                              static_cast<std::uint16_t>(frame[i]));
        }

        if (::send(socket_, packet_.data(), packet_.size(), 0) ==
            static_cast<ssize_t>(packet_.size())) {
            sent_.fetch_add(1, std::memory_order_relaxed);
        } else {
            errors_.fetch_add(1, std::memory_order_relaxed);
        }

        ++sequence_;
        timestamp_ += kFramesPerPacket;
        first_ = false;
    }

    int socket_ = -1;
    const char* error_ = nullptr;
    detail::SampleRing ring_;
    std::thread thread_;
    std::counting_semaphore<> ready_{0};
    std::atomic_bool running_{false};
    std::array<std::uint8_t, kPacketBytes> packet_{};
    std::uint32_t ssrc_;
    std::uint16_t sequence_;
    std::uint32_t timestamp_;
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> errors_{0};
    std::atomic<std::uint64_t> dropped_{0};
    bool first_ = true;
};

// Receives RTP into a fixed-depth jitter buffer.
//
// pull() is what the audio render callback calls: it never blocks, never
// allocates, and zero-fills when the buffer runs dry.
class Receiver {
  public:
    // jitter_packets is the whole latency budget on this side: playback waits
    // for that much audio before it starts, and refills to it after a dropout.
    // 1 packet (10 ms) is the floor; 2 to 3 survives a normal LAN.
    explicit Receiver(std::uint16_t port = kPort, std::uint32_t jitter_packets = 2)
        : target_(jitter_packets * kFramesPerPacket),
          // Headroom above the target absorbs a burst; pull() trims back down
          // to the target so latency cannot creep upwards.
          limit_((jitter_packets + 4) * kFramesPerPacket),
          ring_(kReceiveQueuePackets * kFramesPerPacket) {
        socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_ < 0) {
            error_ = "socket";
            return;
        }

        const int reuse = 1;
        ::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);
        if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
            error_ = "bind";
            return;
        }

        // Lets the receive thread notice stop() without needing a wakeup pipe.
        timeval timeout{};
        timeout.tv_usec = static_cast<suseconds_t>(kStopPollInterval.count() * 1000);
        ::setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }

    ~Receiver() {
        stop();
        if (socket_ >= 0) {
            ::close(socket_);
        }
    }

    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    bool ok() const {
        return error_ == nullptr;
    }

    const char* error() const {
        return error_;
    }

    void start() {
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { receive_loop(); });
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // Called from the audio render callback.
    void pull(std::int16_t* out, std::size_t frames) {
        const std::size_t produced = drain(out, frames);
        std::fill(out + produced, out + frames, static_cast<std::int16_t>(0));
    }

    // Called from the audio render callback.
    void pull(float* out, std::size_t frames) {
        while (frames > 0) {
            const std::size_t chunk = std::min(frames, scratch_.size());
            const std::size_t produced = drain(scratch_.data(), chunk);
            for (std::size_t i = 0; i < chunk; ++i) {
                out[i] = (i < produced) ? static_cast<float>(scratch_[i]) / 32768.0F : 0.0F;
            }
            out += chunk;
            frames -= chunk;
        }
    }

    std::uint64_t packets_received() const {
        return received_.load(std::memory_order_relaxed);
    }

    std::uint64_t packets_lost() const {
        return lost_.load(std::memory_order_relaxed);
    }

    std::uint64_t packets_late() const {
        return late_.load(std::memory_order_relaxed);
    }

    std::uint64_t packets_dropped() const {
        return dropped_.load(std::memory_order_relaxed);
    }

    std::uint64_t underruns() const {
        return underruns_.load(std::memory_order_relaxed);
    }

    std::uint64_t overruns() const {
        return overruns_.load(std::memory_order_relaxed);
    }

  private:
    // Consumer side of the jitter buffer. Returns how many frames it produced;
    // anything short of what was asked for is the caller's to zero.
    std::size_t drain(std::int16_t* out, std::size_t frames) {
        const std::size_t queued = ring_.size();

        if (queued > limit_ + frames) {
            // The sender's clock runs faster than ours, or a burst landed.
            // Give up the excess audio rather than the latency. Trim to one
            // request above the target, so the pop below leaves exactly the
            // target behind -- the same depth the priming path settles on.
            ring_.discard(queued - (target_ + frames));
            overruns_.fetch_add(1, std::memory_order_relaxed);
        }

        if (priming_) {
            if (ring_.size() < target_ + frames) {
                return 0; // still filling; the caller outputs silence
            }
            priming_ = false;
        }

        const std::size_t produced = ring_.pop(out, frames);
        if (produced < frames) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
            priming_ = true; // refill to the target before trying again
        }
        return produced;
    }

    void receive_loop() {
        std::array<std::uint8_t, 2048> datagram{};
        std::array<std::int16_t, kFramesPerPacket> samples{};

        while (running_.load(std::memory_order_acquire)) {
            const ssize_t size = ::recv(socket_, datagram.data(), datagram.size(), 0);
            if (size < static_cast<ssize_t>(kHeaderBytes)) {
                continue; // timeout, or too short to be RTP
            }
            if ((datagram[0] >> 6) != 2 || (datagram[1] & 0x7F) != kPayloadType) {
                continue; // not the stream we are listening for
            }

            const std::uint16_t sequence = detail::read_u16(&datagram[2]);
            const auto payload = static_cast<std::size_t>(size) - kHeaderBytes;
            const std::size_t frames = std::min(payload / sizeof(std::int16_t), samples.size());
            for (std::size_t i = 0; i < frames; ++i) {
                samples[i] = static_cast<std::int16_t>(
                    detail::read_u16(&datagram[kHeaderBytes + i * sizeof(std::int16_t)]));
            }

            received_.fetch_add(1, std::memory_order_relaxed);
            conceal_gap(sequence);
            if (!ring_.push(samples.data(), frames)) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
            }
            expected_ = static_cast<std::uint16_t>(sequence + 1);
        }
    }

    // Keeps the timeline honest: a missing packet becomes the same amount of
    // silence, so audio after a dropout is not shifted earlier.
    void conceal_gap(std::uint16_t sequence) {
        if (!synced_) {
            synced_ = true;
            return;
        }

        const auto gap = static_cast<std::int16_t>(sequence - expected_);
        if (gap < 0) {
            late_.fetch_add(1, std::memory_order_relaxed); // reordered or duplicate
            return;
        }
        if (gap == 0) {
            return;
        }
        if (static_cast<std::uint32_t>(gap) > kMaxConcealPackets) {
            return; // treat as a restart; resync on this packet
        }

        lost_.fetch_add(static_cast<std::uint64_t>(gap), std::memory_order_relaxed);
        ring_.push_silence(static_cast<std::size_t>(gap) * kFramesPerPacket);
    }

    int socket_ = -1;
    const char* error_ = nullptr;
    std::size_t target_;
    std::size_t limit_;
    detail::SampleRing ring_;
    std::array<std::int16_t, kMaxPullFrames> scratch_{}; // audio thread only
    std::thread thread_;
    std::atomic_bool running_{false};
    std::atomic<std::uint64_t> received_{0};
    std::atomic<std::uint64_t> lost_{0};
    std::atomic<std::uint64_t> late_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> overruns_{0};
    std::uint16_t expected_ = 0;
    bool synced_ = false;
    bool priming_ = true;
};

} // namespace rtp

#endif // INTERKOM_EXAMPLES_RTP_HPP
