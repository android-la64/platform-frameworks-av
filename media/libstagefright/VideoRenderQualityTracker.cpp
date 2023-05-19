/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "VideoRenderQualityTracker"
#include <utils/Log.h>

#include <media/stagefright/VideoRenderQualityTracker.h>

#include <assert.h>
#include <cmath>
#include <sys/time.h>

namespace android {

static constexpr float FRAME_RATE_UNDETERMINED = VideoRenderQualityMetrics::FRAME_RATE_UNDETERMINED;
static constexpr float FRAME_RATE_24_3_2_PULLDOWN =
        VideoRenderQualityMetrics::FRAME_RATE_24_3_2_PULLDOWN;

VideoRenderQualityMetrics::VideoRenderQualityMetrics() {
    clear();
}

void VideoRenderQualityMetrics::clear() {
    firstRenderTimeUs = 0;
    frameReleasedCount = 0;
    frameRenderedCount = 0;
    frameDroppedCount = 0;
    frameSkippedCount = 0;
    contentFrameRate = FRAME_RATE_UNDETERMINED;
    desiredFrameRate = FRAME_RATE_UNDETERMINED;
    actualFrameRate = FRAME_RATE_UNDETERMINED;
    freezeDurationMsHistogram.clear();
    freezeDistanceMsHistogram.clear();
    judderScoreHistogram.clear();
}

VideoRenderQualityTracker::Configuration::Configuration() {
    enabled = true;

    // Assume that the app is skipping frames because it's detected that the frame couldn't be
    // rendered in time.
    areSkippedFramesDropped = true;

    // 400ms is 8 frames at 20 frames per second and 24 frames at 60 frames per second
    maxExpectedContentFrameDurationUs = 400 * 1000;

    // Allow for 2 milliseconds of deviation when detecting frame rates
    frameRateDetectionToleranceUs = 2 * 1000;

    // Allow for a tolerance of 200 milliseconds for determining if we moved forward in content time
    // because of frame drops for live content, or because the user is seeking.
    contentTimeAdvancedForLiveContentToleranceUs = 200 * 1000;

    // Freeze configuration
    freezeDurationMsHistogramBuckets = {1, 20, 40, 60, 80, 100, 120, 150, 175, 225, 300, 400, 500};
    freezeDurationMsHistogramToScore = {1,  1,  1,  1,  1,   1,   1,   1,   1,   1,   1,   1,   1};
    freezeDistanceMsHistogramBuckets = {0, 20, 100, 400, 1000, 2000, 3000, 4000, 8000, 15000, 30000,
                                        60000};

    // Judder configuration
    judderErrorToleranceUs = 2000;
    judderScoreHistogramBuckets = {1, 4, 5, 9, 11, 20, 30, 40, 50, 60, 70, 80};
    judderScoreHistogramToScore = {1, 1, 1, 1,  1,  1,  1,  1,  1,  1,  1,  1};
}

VideoRenderQualityTracker::VideoRenderQualityTracker() : mConfiguration(Configuration()) {
    configureHistograms(mMetrics, mConfiguration);
    clear();
}

VideoRenderQualityTracker::VideoRenderQualityTracker(const Configuration &configuration) :
        mConfiguration(configuration) {
    configureHistograms(mMetrics, mConfiguration);
    clear();
}

void VideoRenderQualityTracker::onTunnelFrameQueued(int64_t contentTimeUs) {
    if (!mConfiguration.enabled) {
        return;
    }

    // Since P-frames are queued out of order, hold onto the P-frame until we can track it in
    // render order. This only works because it depends on today's encoding algorithms that only
    // allow B-frames to refer to ONE P-frame that comes after it. If the cardinality of P-frames
    // in a single mini-GOP is increased, this algorithm breaks down.
    if (mTunnelFrameQueuedContentTimeUs == -1) {
        mTunnelFrameQueuedContentTimeUs = contentTimeUs;
    } else if (contentTimeUs < mTunnelFrameQueuedContentTimeUs) {
        onFrameReleased(contentTimeUs, 0);
    } else {
        onFrameReleased(mTunnelFrameQueuedContentTimeUs, 0);
        mTunnelFrameQueuedContentTimeUs = contentTimeUs;
    }
}

void VideoRenderQualityTracker::onFrameSkipped(int64_t contentTimeUs) {
    if (!mConfiguration.enabled) {
        return;
    }

    // Frames skipped at the beginning shouldn't really be counted as skipped frames, since the
    // app might be seeking to a starting point that isn't the first key frame.
    if (mLastRenderTimeUs == -1) {
        return;
    }
    // Frames skipped at the end of playback shouldn't be counted as skipped frames, since the
    // app could be terminating the playback. The pending count will be added to the metrics if and
    // when the next frame is rendered.
    mPendingSkippedFrameContentTimeUsList.push_back(contentTimeUs);
}

void VideoRenderQualityTracker::onFrameReleased(int64_t contentTimeUs) {
    onFrameReleased(contentTimeUs, nowUs() * 1000);
}

void VideoRenderQualityTracker::onFrameReleased(int64_t contentTimeUs,
                                                int64_t desiredRenderTimeNs) {
    if (!mConfiguration.enabled) {
        return;
    }

    int64_t desiredRenderTimeUs = desiredRenderTimeNs / 1000;
    resetIfDiscontinuity(contentTimeUs, desiredRenderTimeUs);
    mMetrics.frameReleasedCount++;
    mNextExpectedRenderedFrameQueue.push({contentTimeUs, desiredRenderTimeUs});
    mLastContentTimeUs = contentTimeUs;
}

void VideoRenderQualityTracker::onFrameRendered(int64_t contentTimeUs, int64_t actualRenderTimeNs) {
    if (!mConfiguration.enabled) {
        return;
    }

    int64_t actualRenderTimeUs = actualRenderTimeNs / 1000;

    if (mLastRenderTimeUs != -1) {
        mRenderDurationMs += (actualRenderTimeUs - mLastRenderTimeUs) / 1000;
    }
    // Now that a frame has been rendered, the previously skipped frames can be processed as skipped
    // frames since the app is not skipping them to terminate playback.
    for (int64_t contentTimeUs : mPendingSkippedFrameContentTimeUsList) {
        processMetricsForSkippedFrame(contentTimeUs);
    }
    mPendingSkippedFrameContentTimeUsList = {};

    // We can render a pending queued frame if it's the last frame of the video, so release it
    // immediately.
    if (contentTimeUs == mTunnelFrameQueuedContentTimeUs && mTunnelFrameQueuedContentTimeUs != -1) {
        onFrameReleased(mTunnelFrameQueuedContentTimeUs, 0);
        mTunnelFrameQueuedContentTimeUs = -1;
    }

    static const FrameInfo noFrame = {-1, -1};
    FrameInfo nextExpectedFrame = noFrame;
    while (!mNextExpectedRenderedFrameQueue.empty()) {
        nextExpectedFrame = mNextExpectedRenderedFrameQueue.front();
        mNextExpectedRenderedFrameQueue.pop();
        // Happy path - the rendered frame is what we expected it to be
        if (contentTimeUs == nextExpectedFrame.contentTimeUs) {
            break;
        }
        // This isn't really supposed to happen - the next rendered frame should be the expected
        // frame, or, if there's frame drops, it will be a frame later in the content stream
        if (contentTimeUs < nextExpectedFrame.contentTimeUs) {
            ALOGW("Rendered frame is earlier than the next expected frame (%lld, %lld)",
                  (long long) contentTimeUs, (long long) nextExpectedFrame.contentTimeUs);
            break;
        }
        processMetricsForDroppedFrame(nextExpectedFrame.contentTimeUs,
                                      nextExpectedFrame.desiredRenderTimeUs);
    }
    processMetricsForRenderedFrame(nextExpectedFrame.contentTimeUs,
                                   nextExpectedFrame.desiredRenderTimeUs, actualRenderTimeUs);
    mLastRenderTimeUs = actualRenderTimeUs;
}

const VideoRenderQualityMetrics &VideoRenderQualityTracker::getMetrics() {
    if (!mConfiguration.enabled) {
        return mMetrics;
    }

    mMetrics.freezeScore = 0;
    if (mConfiguration.freezeDurationMsHistogramToScore.size() ==
        mMetrics.freezeDurationMsHistogram.size()) {
        for (int i = 0; i < mMetrics.freezeDurationMsHistogram.size(); ++i) {
            int32_t count = 0;
            for (int j = i; j < mMetrics.freezeDurationMsHistogram.size(); ++j) {
                count += mMetrics.freezeDurationMsHistogram[j];
            }
            mMetrics.freezeScore += count / mConfiguration.freezeDurationMsHistogramToScore[i];
        }
    }
    mMetrics.freezeRate = float(double(mMetrics.freezeDurationMsHistogram.getSum()) /
            mRenderDurationMs);

    mMetrics.judderScore = 0;
    if (mConfiguration.judderScoreHistogramToScore.size() == mMetrics.judderScoreHistogram.size()) {
        for (int i = 0; i < mMetrics.judderScoreHistogram.size(); ++i) {
            int32_t count = 0;
            for (int j = i; j < mMetrics.judderScoreHistogram.size(); ++j) {
                count += mMetrics.judderScoreHistogram[j];
            }
            mMetrics.judderScore += count / mConfiguration.judderScoreHistogramToScore[i];
        }
    }
    mMetrics.judderRate = float(double(mMetrics.judderScoreHistogram.getCount()) /
            (mMetrics.frameReleasedCount + mMetrics.frameSkippedCount));

    return mMetrics;
}

void VideoRenderQualityTracker::clear() {
    mRenderDurationMs = 0;
    mMetrics.clear();
    resetForDiscontinuity();
}

void VideoRenderQualityTracker::resetForDiscontinuity() {
    mLastContentTimeUs = -1;
    mLastRenderTimeUs = -1;
    mLastFreezeEndTimeUs = -1;

    // Don't worry about tracking frame rendering times from now up until playback catches up to the
    // discontinuity. While stuttering or freezing could be found in the next few frames, the impact
    // to the user is is minimal, so better to just keep things simple and don't bother.
    mNextExpectedRenderedFrameQueue = {};
    mTunnelFrameQueuedContentTimeUs = -1;

    // Ignore any frames that were skipped just prior to the discontinuity.
    mPendingSkippedFrameContentTimeUsList = {};

    // All frame durations can be now ignored since all bets are off now on what the render
    // durations should be after the discontinuity.
    for (int i = 0; i < FrameDurationUs::SIZE; ++i) {
        mActualFrameDurationUs[i] = -1;
        mDesiredFrameDurationUs[i] = -1;
        mContentFrameDurationUs[i] = -1;
    }
}

bool VideoRenderQualityTracker::resetIfDiscontinuity(int64_t contentTimeUs,
                                                     int64_t desiredRenderTimeUs) {
    if (mLastContentTimeUs == -1) {
        resetForDiscontinuity();
        return true;
    }
    if (contentTimeUs < mLastContentTimeUs) {
        ALOGI("Video playback jumped %d ms backwards in content time (%d -> %d)",
              int((mLastContentTimeUs - contentTimeUs) / 1000), int(mLastContentTimeUs / 1000),
              int(contentTimeUs / 1000));
        resetForDiscontinuity();
        return true;
    }
    if (contentTimeUs - mLastContentTimeUs > mConfiguration.maxExpectedContentFrameDurationUs) {
        // The content frame duration could be long due to frame drops for live content. This can be
        // detected by looking at the app's desired rendering duration. If the app's rendered frame
        // duration is roughly the same as the content's frame duration, then it is assumed that
        // the forward discontinuity is due to frame drops for live content. A false positive can
        // occur if the time the user spends seeking is equal to the duration of the seek. This is
        // very unlikely to occur in practice but CAN occur - the user starts seeking forward, gets
        // distracted, and then returns to seeking forward.
        int64_t contentFrameDurationUs = contentTimeUs - mLastContentTimeUs;
        int64_t desiredFrameDurationUs = desiredRenderTimeUs - mLastRenderTimeUs;
        bool skippedForwardDueToLiveContentFrameDrops =
                abs(contentFrameDurationUs - desiredFrameDurationUs) <
                mConfiguration.contentTimeAdvancedForLiveContentToleranceUs;
        if (!skippedForwardDueToLiveContentFrameDrops) {
            ALOGI("Video playback jumped %d ms forward in content time (%d -> %d) ",
                int((contentTimeUs - mLastContentTimeUs) / 1000), int(mLastContentTimeUs / 1000),
                int(contentTimeUs / 1000));
            resetForDiscontinuity();
            return true;
        }
    }
    return false;
}

void VideoRenderQualityTracker::processMetricsForSkippedFrame(int64_t contentTimeUs) {
    mMetrics.frameSkippedCount++;
    if (mConfiguration.areSkippedFramesDropped) {
        processMetricsForDroppedFrame(contentTimeUs, -1);
        return;
    }
    updateFrameDurations(mContentFrameDurationUs, contentTimeUs);
    updateFrameDurations(mDesiredFrameDurationUs, -1);
    updateFrameDurations(mActualFrameDurationUs, -1);
    updateFrameRate(mMetrics.contentFrameRate, mContentFrameDurationUs, mConfiguration);
}

void VideoRenderQualityTracker::processMetricsForDroppedFrame(int64_t contentTimeUs,
                                                              int64_t desiredRenderTimeUs) {
    mMetrics.frameDroppedCount++;
    updateFrameDurations(mContentFrameDurationUs, contentTimeUs);
    updateFrameDurations(mDesiredFrameDurationUs, desiredRenderTimeUs);
    updateFrameDurations(mActualFrameDurationUs, -1);
    updateFrameRate(mMetrics.contentFrameRate, mContentFrameDurationUs, mConfiguration);
    updateFrameRate(mMetrics.desiredFrameRate, mDesiredFrameDurationUs, mConfiguration);
}

void VideoRenderQualityTracker::processMetricsForRenderedFrame(int64_t contentTimeUs,
                                                               int64_t desiredRenderTimeUs,
                                                               int64_t actualRenderTimeUs) {
    // Capture the timestamp at which the first frame was rendered
    if (mMetrics.firstRenderTimeUs == 0) {
        mMetrics.firstRenderTimeUs = actualRenderTimeUs;
    }

    mMetrics.frameRenderedCount++;

    // The content time is -1 when it was rendered after a discontinuity (e.g. seek) was detected.
    // So, even though a frame was rendered, it's impact on the user is insignificant, so don't do
    // anything other than count it as a rendered frame.
    if (contentTimeUs == -1) {
        return;
    }
    updateFrameDurations(mContentFrameDurationUs, contentTimeUs);
    updateFrameDurations(mDesiredFrameDurationUs, desiredRenderTimeUs);
    updateFrameDurations(mActualFrameDurationUs, actualRenderTimeUs);
    updateFrameRate(mMetrics.contentFrameRate, mContentFrameDurationUs, mConfiguration);
    updateFrameRate(mMetrics.desiredFrameRate, mDesiredFrameDurationUs, mConfiguration);
    updateFrameRate(mMetrics.actualFrameRate, mActualFrameDurationUs, mConfiguration);

    // If the previous frame was dropped, there was a freeze if we've already rendered a frame
    if (mActualFrameDurationUs[1] == -1 && mLastRenderTimeUs != -1) {
        processFreeze(actualRenderTimeUs, mLastRenderTimeUs, mLastFreezeEndTimeUs, mMetrics);
        mLastFreezeEndTimeUs = actualRenderTimeUs;
    }

    // Judder is computed on the prior video frame, not the current video frame
    int64_t judderScore = computePreviousJudderScore(mActualFrameDurationUs,
                                                     mContentFrameDurationUs,
                                                     mConfiguration);
    if (judderScore != 0) {
        mMetrics.judderScoreHistogram.insert(judderScore);
    }
}

void VideoRenderQualityTracker::processFreeze(int64_t actualRenderTimeUs, int64_t lastRenderTimeUs,
                                              int64_t lastFreezeEndTimeUs,
                                              VideoRenderQualityMetrics &m) {
    int64_t freezeDurationMs = (actualRenderTimeUs - lastRenderTimeUs) / 1000;
    m.freezeDurationMsHistogram.insert(freezeDurationMs);
    if (lastFreezeEndTimeUs != -1) {
        int64_t distanceSinceLastFreezeMs = (lastRenderTimeUs - lastFreezeEndTimeUs) / 1000;
        m.freezeDistanceMsHistogram.insert(distanceSinceLastFreezeMs);
    }
}

int64_t VideoRenderQualityTracker::computePreviousJudderScore(
        const FrameDurationUs &actualFrameDurationUs,
        const FrameDurationUs &contentFrameDurationUs,
        const Configuration &c) {
    // If the frame before or after was dropped, then don't generate a judder score, since any
    // problems with frame drops are scored as a freeze instead.
    if (actualFrameDurationUs[0] == -1 || actualFrameDurationUs[1] == -1 ||
        actualFrameDurationUs[2] == -1) {
        return 0;
    }

    // Don't score judder for when playback is paused or rebuffering (long frame duration), or if
    // the player is intentionally playing each frame at a slow rate (e.g. half-rate). If the long
    // frame duration was unintentional, it is assumed that this will be coupled with a later frame
    // drop, and be scored as a freeze instead of judder.
    if (actualFrameDurationUs[1] >= 2 * contentFrameDurationUs[1]) {
        return 0;
    }

    // The judder score is based on the error of this frame
    int64_t errorUs = actualFrameDurationUs[1] - contentFrameDurationUs[1];
    // Don't score judder if the previous frame has high error, but this frame has low error
    if (abs(errorUs) < c.judderErrorToleranceUs) {
        return 0;
    }

    // Add a penalty if this frame has judder that amplifies the problem introduced by previous
    // judder, instead of catching up for the previous judder (50, 16, 16, 50) vs (50, 16, 50, 16)
    int64_t previousErrorUs = actualFrameDurationUs[2] - contentFrameDurationUs[2];
    // Don't add the pentalty for errors from the previous frame if the previous frame has low error
    if (abs(previousErrorUs) >= c.judderErrorToleranceUs) {
        errorUs = abs(errorUs) + abs(errorUs + previousErrorUs);
    }

    // Avoid scoring judder for 3:2 pulldown or other minimally-small frame duration errors
    if (abs(errorUs) < contentFrameDurationUs[1] / 4) {
        return 0;
    }

    return abs(errorUs) / 1000; // error in millis to keep numbers small
}

void VideoRenderQualityTracker::configureHistograms(VideoRenderQualityMetrics &m,
                                                    const Configuration &c) {
    m.freezeDurationMsHistogram.setup(c.freezeDurationMsHistogramBuckets);
    m.freezeDistanceMsHistogram.setup(c.freezeDistanceMsHistogramBuckets);
    m.judderScoreHistogram.setup(c.judderScoreHistogramBuckets);
}

int64_t VideoRenderQualityTracker::nowUs() {
    struct timespec t;
    t.tv_sec = t.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec * 1000000000LL + t.tv_nsec) / 1000LL;
}

void VideoRenderQualityTracker::updateFrameDurations(FrameDurationUs &durationUs,
                                                     int64_t newTimestampUs) {
    for (int i = FrameDurationUs::SIZE - 1; i > 0; --i ) {
        durationUs[i] = durationUs[i - 1];
    }
    if (newTimestampUs == -1) {
        durationUs[0] = -1;
    } else {
        durationUs[0] = durationUs.priorTimestampUs == -1 ? -1 :
                newTimestampUs - durationUs.priorTimestampUs;
        durationUs.priorTimestampUs = newTimestampUs;
    }
}

void VideoRenderQualityTracker::updateFrameRate(float &frameRate, const FrameDurationUs &durationUs,
                                                const Configuration &c) {
    float newFrameRate = detectFrameRate(durationUs, c);
    if (newFrameRate != FRAME_RATE_UNDETERMINED) {
        frameRate = newFrameRate;
    }
}

float VideoRenderQualityTracker::detectFrameRate(const FrameDurationUs &durationUs,
                                                 const Configuration &c) {
    // At least 3 frames are necessary to detect stable frame rates
    assert(FrameDurationUs::SIZE >= 3);
    if (durationUs[0] == -1 || durationUs[1] == -1 || durationUs[2] == -1) {
        return FRAME_RATE_UNDETERMINED;
    }
    // Only determine frame rate if the render durations are stable across 3 frames
    if (abs(durationUs[0] - durationUs[1]) > c.frameRateDetectionToleranceUs ||
        abs(durationUs[0] - durationUs[2]) > c.frameRateDetectionToleranceUs) {
        return is32pulldown(durationUs, c) ? FRAME_RATE_24_3_2_PULLDOWN : FRAME_RATE_UNDETERMINED;
    }
    return 1000.0 * 1000.0 / durationUs[0];
}

bool VideoRenderQualityTracker::is32pulldown(const FrameDurationUs &durationUs,
                                             const Configuration &c) {
    // At least 5 frames are necessary to detect stable 3:2 pulldown
    assert(FrameDurationUs::SIZE >= 5);
    if (durationUs[0] == -1 || durationUs[1] == -1 || durationUs[2] == -1 || durationUs[3] == -1 ||
        durationUs[4] == -1) {
        return false;
    }
    // 3:2 pulldown expects that every other frame has identical duration...
    if (abs(durationUs[0] - durationUs[2]) > c.frameRateDetectionToleranceUs ||
        abs(durationUs[1] - durationUs[3]) > c.frameRateDetectionToleranceUs ||
        abs(durationUs[0] - durationUs[4]) > c.frameRateDetectionToleranceUs) {
        return false;
    }
    // ... for either 2 vsysncs or 3 vsyncs
    if ((abs(durationUs[0] - 33333) < c.frameRateDetectionToleranceUs &&
         abs(durationUs[1] - 50000) < c.frameRateDetectionToleranceUs) ||
        (abs(durationUs[0] - 50000) < c.frameRateDetectionToleranceUs &&
         abs(durationUs[1] - 33333) < c.frameRateDetectionToleranceUs)) {
        return true;
    }
    return false;
}

} // namespace android
