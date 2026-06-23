#include "audio_call_session.hpp"

#include <QByteArray>
#include <QTimer>

#if LAN_CHAT_GUI_ENABLE_AV
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QDateTime>
#include <QIODevice>
#include <QMediaDevices>

#include <array>
#include <chrono>
#include <memory>
#include <vector>

#include <opus/opus.h>
#include <rtc/rtc.hpp>
#endif

class AudioCallSession::Impl {
public:
    explicit Impl(AudioCallSession *owner)
        : owner(owner)
    {
    }

    virtual ~Impl() = default;
    virtual void startCaller() = 0;
    virtual void startCallee() = 0;
    virtual void applyRemoteOffer(const QString &sdp) = 0;
    virtual void applyRemoteAnswer(const QString &sdp) = 0;
    virtual void applyRemoteIce(const QString &candidate, const QString &mid, int mlineIndex) = 0;
    virtual void setMicrophoneMuted(bool muted) = 0;
    virtual void stop() = 0;
    virtual bool enabled() const = 0;
    virtual QString state() const = 0;

protected:
    void publishState(const QString &state)
    {
        state_ = state;
        emit owner->stateChanged(state_);
    }

    AudioCallSession *owner{nullptr};
    QString state_{QStringLiteral("idle")};
};

namespace {

class NoopAudioCallSession final : public AudioCallSession::Impl {
public:
    explicit NoopAudioCallSession(AudioCallSession *owner)
        : Impl(owner)
    {
    }

    void startCaller() override
    {
        publishState(QStringLiteral("signaling-only"));
        emit owner->errorOccurred(QStringLiteral("AV media is disabled in this build"));
    }

    void startCallee() override
    {
        publishState(QStringLiteral("signaling-only"));
    }

    void applyRemoteOffer(const QString &) override
    {
        emit owner->errorOccurred(QStringLiteral("AV media is disabled in this build"));
    }

    void applyRemoteAnswer(const QString &) override {}
    void applyRemoteIce(const QString &, const QString &, int) override {}
    void setMicrophoneMuted(bool) override {}
    void stop() override { publishState(QStringLiteral("idle")); }
    bool enabled() const override { return false; }
    QString state() const override { return state_; }
};

#if LAN_CHAT_GUI_ENABLE_AV

constexpr std::uint8_t kAudioPayloadType = 111;
constexpr int kSampleRate = 48000;
constexpr int kChannels = 1;
constexpr int kFrameMs = 20;
constexpr int kSamplesPerFrame = kSampleRate * kFrameMs / 1000;
constexpr int kBytesPerSample = 2;
constexpr int kBytesPerFrame = kSamplesPerFrame * kChannels * kBytesPerSample;
constexpr int kMaxOpusPacket = 4000;

QAudioFormat preferredAudioFormat()
{
    QAudioFormat format;
    format.setSampleRate(kSampleRate);
    format.setChannelCount(kChannels);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}

class RealAudioCallSession final : public AudioCallSession::Impl {
public:
    explicit RealAudioCallSession(AudioCallSession *owner)
        : Impl(owner)
    {
        rtc::InitLogger(rtc::LogLevel::Warning);
        config_.disableAutoNegotiation = false;
        config_.enableIceTcp = true;
        config_.iceServers.clear();
        initOpus();
        playbackPump_.setInterval(10);
        QObject::connect(&playbackPump_, &QTimer::timeout, owner, [this]() {
            flushRemoteAudio();
        });
    }

    ~RealAudioCallSession() override
    {
        stop();
        releaseOpus();
    }

    void startCaller() override
    {
        ensurePeerConnection();
        startAudioDevices();
        if (peerConnection_) {
            peerConnection_->setLocalDescription();
            publishState(QStringLiteral("signaling"));
        }
    }

    void startCallee() override
    {
        ensurePeerConnection();
        startAudioDevices();
        publishState(QStringLiteral("signaling"));
    }

    void applyRemoteOffer(const QString &sdp) override
    {
        ensurePeerConnection();
        startAudioDevices();
        if (!peerConnection_) {
            return;
        }
        peerConnection_->setRemoteDescription(rtc::Description(sdp.toUtf8().toStdString(), "offer"));
        remoteDescriptionApplied_ = true;
        flushPendingCandidates();
        peerConnection_->setLocalDescription();
        publishState(QStringLiteral("connecting"));
    }

    void applyRemoteAnswer(const QString &sdp) override
    {
        if (!peerConnection_) {
            emit owner->errorOccurred(QStringLiteral("peer connection is not initialized"));
            return;
        }
        peerConnection_->setRemoteDescription(rtc::Description(sdp.toUtf8().toStdString(), "answer"));
        remoteDescriptionApplied_ = true;
        flushPendingCandidates();
        publishState(QStringLiteral("connecting"));
    }

    void applyRemoteIce(const QString &candidate, const QString &mid, int mlineIndex) override
    {
        Q_UNUSED(mlineIndex);
        if (!peerConnection_) {
            pendingCandidates_.push_back({candidate, mid, 0});
            return;
        }
        if (!remoteDescriptionApplied_) {
            pendingCandidates_.push_back({candidate, mid, 0});
            return;
        }
        peerConnection_->addRemoteCandidate(rtc::Candidate(candidate.toUtf8().toStdString(), mid.toUtf8().toStdString()));
    }

    void setMicrophoneMuted(bool muted) override
    {
        microphoneMuted_ = muted;
    }

    void stop() override
    {
        playbackPump_.stop();
        remotePlaybackBuffer_.clear();
        audioInputDevice_ = nullptr;
        audioOutputDevice_ = nullptr;
        audioSource_.reset();
        audioSink_.reset();
        captureBuffer_.clear();
        remoteDescriptionApplied_ = false;
        pendingCandidates_.clear();
        audioTrackOpen_ = false;
        audioTrack_.reset();
        audioRtpConfig_.reset();
        if (peerConnection_) {
            peerConnection_->close();
        }
        peerConnection_.reset();
        publishState(QStringLiteral("idle"));
    }

    bool enabled() const override { return true; }
    QString state() const override { return state_; }

private:
    struct PendingCandidate {
        QString candidate;
        QString mid;
        int mlineIndex{0};
    };

    void ensurePeerConnection()
    {
        if (peerConnection_) {
            return;
        }

        peerConnection_ = std::make_shared<rtc::PeerConnection>(config_);
        setupAudioTrack();
        peerConnection_->onTrack([this](const std::shared_ptr<rtc::Track> &track) {
            setupIncomingTrack(track);
        });
        peerConnection_->onLocalDescription([this](rtc::Description description) {
            const QString sdp = QString::fromUtf8(std::string(description).c_str());
            if (description.type() == rtc::Description::Type::Offer) {
                emit owner->localOfferReady(sdp);
            } else if (description.type() == rtc::Description::Type::Answer) {
                emit owner->localAnswerReady(sdp);
            }
        });
        peerConnection_->onLocalCandidate([this](rtc::Candidate candidate) {
            emit owner->localIceReady(
                QString::fromUtf8(candidate.candidate().c_str()),
                QString::fromUtf8(candidate.mid().c_str()),
                0);
        });
        peerConnection_->onStateChange([this](rtc::PeerConnection::State state) {
            switch (state) {
            case rtc::PeerConnection::State::New:
                publishState(QStringLiteral("new"));
                break;
            case rtc::PeerConnection::State::Connecting:
                publishState(QStringLiteral("connecting"));
                break;
            case rtc::PeerConnection::State::Connected:
                publishState(QStringLiteral("connected"));
                break;
            case rtc::PeerConnection::State::Disconnected:
                publishState(QStringLiteral("disconnected"));
                break;
            case rtc::PeerConnection::State::Failed:
                publishState(QStringLiteral("failed"));
                emit owner->errorOccurred(QStringLiteral("peer connection failed"));
                break;
            case rtc::PeerConnection::State::Closed:
                publishState(QStringLiteral("closed"));
                break;
            }
        });
    }

    void setupAudioTrack()
    {
        const std::string mid = "audio";
        constexpr rtc::SSRC ssrc = 2;
        rtc::Description::Audio audio(mid, rtc::Description::Direction::SendRecv);
        audio.addOpusCodec(kAudioPayloadType);
        audio.addSSRC(ssrc, mid, "stream1", mid);
        audioTrack_ = peerConnection_->addTrack(audio);
        audioRtpConfig_ = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc,
            mid,
            kAudioPayloadType,
            rtc::OpusRtpPacketizer::DefaultClockRate);
        auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(audioRtpConfig_);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(audioRtpConfig_));
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        packetizer->addToChain(std::make_shared<rtc::OpusRtpDepacketizer>());
        packetizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        audioTrack_->setMediaHandler(packetizer);
        audioTrack_->onOpen([this]() {
            audioTrackOpen_ = true;
        });
        audioTrack_->onFrame([this](rtc::binary frame, rtc::FrameInfo) {
            decodeRemoteAudio(frame);
        });
    }

    void setupIncomingTrack(const std::shared_ptr<rtc::Track> &track)
    {
        if (track->description().type() != "audio") {
            return;
        }
        track->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
        track->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
        track->onFrame([this](rtc::binary frame, rtc::FrameInfo) {
            decodeRemoteAudio(frame);
        });
    }

    void startAudioDevices()
    {
        const QAudioFormat format = preferredAudioFormat();
        if (!audioSource_) {
            QAudioDevice input = QMediaDevices::defaultAudioInput();
            if (input.isNull()) {
                emit owner->errorOccurred(QStringLiteral("no microphone device"));
            } else {
                audioSource_ = std::make_unique<QAudioSource>(input, format);
                audioInputDevice_ = audioSource_->start();
                if (audioInputDevice_ != nullptr) {
                    QObject::connect(audioInputDevice_, &QIODevice::readyRead, owner, [this]() {
                        if (audioInputDevice_ == nullptr || microphoneMuted_) {
                            return;
                        }
                        encodeAndSendAudio(audioInputDevice_->readAll());
                    });
                }
            }
        }
        if (!audioSink_) {
            QAudioDevice output = QMediaDevices::defaultAudioOutput();
            if (output.isNull()) {
                emit owner->errorOccurred(QStringLiteral("no speaker device"));
            } else {
                audioSink_ = std::make_unique<QAudioSink>(output, format);
                audioOutputDevice_ = audioSink_->start();
                if (audioOutputDevice_ != nullptr) {
                    playbackPump_.start();
                }
            }
        }
    }

    void encodeAndSendAudio(const QByteArray &pcm)
    {
        if (pcm.isEmpty() || opusEncoder_ == nullptr || !audioTrack_ || !audioTrackOpen_) {
            return;
        }
        captureBuffer_.append(pcm);
        while (captureBuffer_.size() >= kBytesPerFrame) {
            const QByteArray frame = captureBuffer_.first(kBytesPerFrame);
            captureBuffer_.remove(0, kBytesPerFrame);
            std::array<unsigned char, kMaxOpusPacket> encoded{};
            const auto *samples = reinterpret_cast<const opus_int16 *>(frame.constData());
            const int encodedBytes = opus_encode(
                static_cast<OpusEncoder *>(opusEncoder_),
                samples,
                kSamplesPerFrame,
                encoded.data(),
                static_cast<opus_int32>(encoded.size()));
            if (encodedBytes <= 0) {
                emit owner->errorOccurred(QStringLiteral("Opus encode failed"));
                return;
            }
            const double timestampUs = static_cast<double>(localAudioFramesSent_ * kFrameMs) * 1000.0;
            audioTrack_->sendFrame(
                reinterpret_cast<const std::byte *>(encoded.data()),
                static_cast<size_t>(encodedBytes),
                std::chrono::duration<double, std::micro>(timestampUs));
            ++localAudioFramesSent_;
        }
    }

    void decodeRemoteAudio(const rtc::binary &frame)
    {
        if (opusDecoder_ == nullptr || frame.empty()) {
            return;
        }
        std::vector<opus_int16> decoded(kSampleRate / 10);
        const int decodedSamples = opus_decode(
            static_cast<OpusDecoder *>(opusDecoder_),
            reinterpret_cast<const unsigned char *>(frame.data()),
            static_cast<opus_int32>(frame.size()),
            decoded.data(),
            static_cast<int>(decoded.size()),
            0);
        if (decodedSamples <= 0) {
            emit owner->errorOccurred(QStringLiteral("Opus decode failed"));
            return;
        }
        const int decodedBytes = decodedSamples * kChannels * kBytesPerSample;
        remotePlaybackBuffer_.append(reinterpret_cast<const char *>(decoded.data()), decodedBytes);
        flushRemoteAudio();
    }

    void flushRemoteAudio()
    {
        if (audioOutputDevice_ == nullptr || remotePlaybackBuffer_.isEmpty()) {
            return;
        }
        const qint64 writable = audioSink_ ? audioSink_->bytesFree() : remotePlaybackBuffer_.size();
        if (writable <= 0) {
            return;
        }
        const qsizetype bytesToWrite = std::min<qsizetype>(remotePlaybackBuffer_.size(), static_cast<qsizetype>(writable));
        const qint64 written = audioOutputDevice_->write(remotePlaybackBuffer_.constData(), bytesToWrite);
        if (written > 0) {
            remotePlaybackBuffer_.remove(0, static_cast<qsizetype>(written));
        }
    }

    void flushPendingCandidates()
    {
        if (!peerConnection_ || !remoteDescriptionApplied_) {
            return;
        }
        const auto candidates = pendingCandidates_;
        pendingCandidates_.clear();
        for (const PendingCandidate &candidate : candidates) {
            Q_UNUSED(candidate.mlineIndex);
            peerConnection_->addRemoteCandidate(
                rtc::Candidate(candidate.candidate.toUtf8().toStdString(), candidate.mid.toUtf8().toStdString()));
        }
    }

    void initOpus()
    {
        int error = OPUS_OK;
        opusEncoder_ = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &error);
        if (error != OPUS_OK) {
            opusEncoder_ = nullptr;
            emit owner->errorOccurred(QStringLiteral("failed to create Opus encoder"));
        }
        error = OPUS_OK;
        opusDecoder_ = opus_decoder_create(kSampleRate, kChannels, &error);
        if (error != OPUS_OK) {
            opusDecoder_ = nullptr;
            emit owner->errorOccurred(QStringLiteral("failed to create Opus decoder"));
        }
    }

    void releaseOpus()
    {
        if (opusEncoder_ != nullptr) {
            opus_encoder_destroy(static_cast<OpusEncoder *>(opusEncoder_));
            opusEncoder_ = nullptr;
        }
        if (opusDecoder_ != nullptr) {
            opus_decoder_destroy(static_cast<OpusDecoder *>(opusDecoder_));
            opusDecoder_ = nullptr;
        }
    }

    rtc::Configuration config_;
    std::shared_ptr<rtc::PeerConnection> peerConnection_;
    std::shared_ptr<rtc::Track> audioTrack_;
    std::shared_ptr<rtc::RtpPacketizationConfig> audioRtpConfig_;
    std::unique_ptr<QAudioSource> audioSource_;
    std::unique_ptr<QAudioSink> audioSink_;
    QIODevice *audioInputDevice_{nullptr};
    QIODevice *audioOutputDevice_{nullptr};
    QByteArray captureBuffer_;
    QByteArray remotePlaybackBuffer_;
    QTimer playbackPump_;
    void *opusEncoder_{nullptr};
    void *opusDecoder_{nullptr};
    quint64 localAudioFramesSent_{0};
    bool audioTrackOpen_{false};
    bool remoteDescriptionApplied_{false};
    bool microphoneMuted_{false};
    std::vector<PendingCandidate> pendingCandidates_;
};

#endif

} // namespace

AudioCallSession::AudioCallSession(QObject *parent)
    : QObject(parent)
{
#if LAN_CHAT_GUI_ENABLE_AV
    impl_ = new RealAudioCallSession(this);
#else
    impl_ = new NoopAudioCallSession(this);
#endif
}

AudioCallSession::~AudioCallSession()
{
    delete impl_;
    impl_ = nullptr;
}

void AudioCallSession::startCaller()
{
    impl_->startCaller();
}

void AudioCallSession::startCallee()
{
    impl_->startCallee();
}

void AudioCallSession::applyRemoteOffer(const QString &sdp)
{
    impl_->applyRemoteOffer(sdp);
}

void AudioCallSession::applyRemoteAnswer(const QString &sdp)
{
    impl_->applyRemoteAnswer(sdp);
}

void AudioCallSession::applyRemoteIce(const QString &candidate, const QString &mid, int mlineIndex)
{
    impl_->applyRemoteIce(candidate, mid, mlineIndex);
}

void AudioCallSession::setMicrophoneMuted(bool muted)
{
    impl_->setMicrophoneMuted(muted);
}

void AudioCallSession::stop()
{
    impl_->stop();
}

bool AudioCallSession::enabled() const
{
    return impl_->enabled();
}

QString AudioCallSession::state() const
{
    return impl_->state();
}
