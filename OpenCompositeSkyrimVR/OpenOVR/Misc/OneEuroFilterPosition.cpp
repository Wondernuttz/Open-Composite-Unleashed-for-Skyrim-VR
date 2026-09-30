#include <algorithm>
#include <corecrt_math_defines.h>
#include <glm/glm.hpp> // Assuming you're using the glm library

class OneEuroFilterWithVelocity {
private:
	double freq;
	double mincutoff;
	double beta_;
	double dcutoff;
	double x_hat_prev;
	double dx;
	bool initialized;

	double alpha(double cutoff) const
	{
		double tau = 1.0 / (2.0 * M_PI * cutoff);
		double te = 1.0 / freq;
		return 1.0 / (1.0 + tau / te);
	}

public:
	OneEuroFilterWithVelocity(double freq = 90.0, double mincutoff = 1.0, double beta = 0.0, double dcutoff = 1.0)
	    : freq(freq), mincutoff(mincutoff), beta_(beta), dcutoff(dcutoff), initialized(false) {}

	double filter(double x, double velocity)
	{
		if (!initialized) {
			x_hat_prev = x;
			dx = velocity;
			initialized = true;
		}

		// Adjust the cutoff frequency based on the velocity
		double adjusted_cutoff = mincutoff + beta_ * std::fabs(velocity);
		double alpha_ = alpha(adjusted_cutoff);

		double x_hat = x_hat_prev + alpha_ * (x - x_hat_prev);
		x_hat_prev = x_hat;

		return x_hat;
	}

	void setFreq(double newFreq)
	{
		freq = newFreq;
	}
};

class OneEuroFilterPosition {
private:
	OneEuroFilterWithVelocity xFilter, yFilter, zFilter;

public:
	OneEuroFilterPosition(double freq = 90.0, double mincutoff = 1, double beta = 0.0, double dcutoff = 1.0)
	    : xFilter(freq, mincutoff, beta, dcutoff), yFilter(freq, mincutoff, beta, dcutoff), zFilter(freq, mincutoff, beta, dcutoff) {}

	glm::vec3 filter(const glm::vec3& position, const glm::vec3& velocity)
	{
		return glm::vec3(
		    xFilter.filter(position.x, velocity.x),
		    yFilter.filter(position.y, velocity.y),
		    zFilter.filter(position.z, velocity.z));
	}

	void setFreq(double freq)
	{
		xFilter.setFreq(freq);
		yFilter.setFreq(freq);
		zFilter.setFreq(freq);
	}
};
