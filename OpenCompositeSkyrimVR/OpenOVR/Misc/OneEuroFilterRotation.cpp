#include <cmath>
#include <corecrt_math_defines.h>

class Quaternion {
public:
	float x, y, z, w;

	Quaternion(float x = 0, float y = 0, float z = 0, float w = 1)
	    : x(x), y(y), z(z), w(w) {}

	Quaternion operator-(const Quaternion& other) const
	{
		return Quaternion(x - other.x, y - other.y, z - other.z, w - other.w);
	}

	Quaternion operator-() const
	{
		return Quaternion(-x, -y, -z, -w);
	}

	float norm() const
	{
		return sqrt(x * x + y * y + z * z + w * w);
	}
};

class LowPassFilter {
	float y, a, s;
	bool initialized;

public:
	LowPassFilter(float alpha = 0.0, float initval = 0.0f)
	    : y(initval), a(alpha), s(initval), initialized(false) {}

	float filter(float value, float alpha)
	{
		float result = initialized ? alpha * value + (1.0f - alpha) * s : value;
		y = value;
		s = result;
		initialized = true;
		return result;
	}
};

class OneEuroFilterRotation {
	float freq;
	float mincutoff;
	float beta;
	float dcutoff;
	LowPassFilter x[4];
	LowPassFilter dx[4];
	Quaternion currValue;

	float alpha(float cutoff) const
	{
		float te = 1.0f / freq;
		float tau = 1.0f / (2.0f * M_PI * cutoff);
		return 1.0f / (1.0f + tau / te);
	}

public:
	OneEuroFilterRotation(float _freq = 90.0f, float _mincutoff = 1.0f, float _beta = 0.0f, float _dcutoff = 1.0f)
	    : freq(_freq), mincutoff(_mincutoff), beta(_beta), dcutoff(_dcutoff)
	{
		for (int i = 0; i < 4; i++) {
			x[i] = LowPassFilter(alpha(mincutoff));
			dx[i] = LowPassFilter(alpha(dcutoff));
		}
	}

	Quaternion filter(Quaternion& value, float wx, float wy, float wz, float timestamp = -1.0f)
	{
		// Handle quaternion double-cover problem
		if ((currValue - value).norm() > sqrt(2)) {
			value = -value;
		}

		float angular_velocity_magnitude = sqrt(wx * wx + wy * wy + wz * wz);
		float cutoff = mincutoff + beta * angular_velocity_magnitude;
		float dynamic_alpha = alpha(cutoff); // compute the dynamic alpha

		currValue = Quaternion(
		    x[0].filter(value.x, dynamic_alpha),
		    x[1].filter(value.y, dynamic_alpha),
		    x[2].filter(value.z, dynamic_alpha),
		    x[3].filter(value.w, dynamic_alpha));

		return currValue;
	}

	void setFreq(double newFreq)
	{
		freq = newFreq;
	}
};