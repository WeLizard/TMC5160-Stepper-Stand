#pragma once
// One inventory for JSON serialization and strict configuration input.
#define CONFIG_FIELDS(X) \
 X(gear) X(rsense) X(hold) X(speed) X(accel) X(decel) X(low) X(high) \
 X(homeCoordinate) X(homeSpeed) X(travel) X(backoff) X(sgMinSpeed) X(encoderTolerance) \
 X(homeTimeout) X(encoderZero) X(fullSteps) X(microsteps) X(current) X(homeCurrent) \
 X(present) X(commissioned) X(hallMask) X(hardLimits) X(homeMethod) X(homeHall) \
 X(sgTuned) X(reverse) X(encoder) X(frameConfirmed) X(zeroValid) X(encoderReverse) \
 X(frameBits) X(stBits) X(mtBits) X(shift) X(grayCode) X(sgt)
