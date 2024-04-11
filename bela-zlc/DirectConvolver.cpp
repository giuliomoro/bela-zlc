/***** DirectConvolver.cpp *****/

#include "DirectConvolver.h"

// Constructor taking the path of a file to load
DirectConvolver::DirectConvolver(std::vector<float> &h, int k, std::vector<float> &x, std::vector<float> &y)
{
	setup(h, k, x, y);
}

// Load an audio file from the given filename. Returns true on success.
bool DirectConvolver::setup(std::vector<float> &h, int k, std::vector<float> &x, std::vector<float> &y)
{
	// store public member values
	k_ = k;
	x_ = &x;
	y_ = &y;
	outPointer_ = k_;

	std::vector<std::vector<float>> hs(1);
	hs[0]  = h;
	int ret = cv.setup(hs, h.size());
	cvIn.reserve(h.size());
	cvOut.resize(h.size());
	return !ret;
}

// Apply the filter h to an input sample in the time domain
void DirectConvolver::process(unsigned int inPointer)
{
	return;
	// receive one sample at a time and keep a copy
	cvIn.push_back(x_->data()[inPointer]);
	if(cvIn.capacity() == cvIn.size())
	{
		// once the buffer is full, perform the convolution for the whole block
		cv.process(cvOut.data(), cvIn.data(), cvIn.size());
		cvIn.resize(0);
		// add the result of the convolution into the output buffer
		for(size_t n = 0; n < cvOut.size(); ++n)
		{
			// write output sample to the output circular buffer
			int circularBufferIndex = (outPointer_ + y_->size()) % y_->size();
			y_->data()[circularBufferIndex] += cvOut[n];
			// update the write pointer one sample ahead
			outPointer_ = (outPointer_ + 1) % y_->size();
		}
	}
}
