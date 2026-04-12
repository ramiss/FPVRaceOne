#ifndef KALMAN_H
#define KALMAN_H

#include <stdint.h>

class KalmanFilter {
   public:
    KalmanFilter();
    float filter(uint16_t z, uint16_t u);
    float lastMeasurement();
    void setMeasurementNoise(float noise);
    void setProcessNoise(float noise);

   private:
    float Q;  // process noise variance  (how much the state can change per step)
    float R;  // measurement noise variance (how noisy the sensor reading is)
    float A;
    float C;
    float B;
    float cov;  // NaN
    float x;    // NaN -- estimated signal without noise
};

#endif
