#include "kalman.h"

#include <math.h>

KalmanFilter::KalmanFilter() {
    Q = 1;  // process noise variance
    R = 1;  // measurement noise variance
    A = 1;
    B = 0;
    C = 1;

    // Ensure the filter starts in a known "uninitialized" state.
    // If x/cov are left uninitialized, they may contain garbage and the
    // filter can output incorrect values (which can cause false triggers).
    x = NAN;
    cov = NAN;
}

float KalmanFilter::filter(uint16_t z, uint16_t u = 0) {
    if (isnan(x)) {
        x = (float)z;
        cov = Q;  // initial uncertainty = process noise variance
    } else {
        // Prediction: propagate state and grow covariance by process noise
        const float predX   = x;          // A=1, B=0
        const float predCov = cov + Q;    // Q = process noise

        // Kalman gain: high R (noisy sensor) → low gain → more smoothing
        const float K = predCov / (predCov + R);  // R = measurement noise

        // Correction
        x   = predX + K * (z - predX);
        cov = predCov * (1.0f - K);
    }

    return x;
}

float KalmanFilter::lastMeasurement() {
    return x;
}

void KalmanFilter::setMeasurementNoise(float noise) {
    R = noise;  // higher R → trust sensor less → more smoothing
}

void KalmanFilter::setProcessNoise(float noise) {
    Q = noise;  // higher Q → state changes faster → less smoothing
}
