#pragma once

#include <QObject>
#include <QString>

class AudioCallSession final : public QObject {
    Q_OBJECT

public:
    class Impl;

    explicit AudioCallSession(QObject *parent = nullptr);
    ~AudioCallSession() override;

    void startCaller();
    void startCallee();
    void applyRemoteOffer(const QString &sdp);
    void applyRemoteAnswer(const QString &sdp);
    void applyRemoteIce(const QString &candidate, const QString &mid, int mlineIndex);
    void setMicrophoneMuted(bool muted);
    void stop();
    [[nodiscard]] bool enabled() const;
    [[nodiscard]] QString state() const;

signals:
    void localOfferReady(const QString &sdp);
    void localAnswerReady(const QString &sdp);
    void localIceReady(const QString &candidate, const QString &mid, int mlineIndex);
    void stateChanged(const QString &state);
    void errorOccurred(const QString &message);

private:
    Impl *impl_{nullptr};
};
