#include <Bela.h>
#include <libraries/Scope/Scope.h>
#include <libraries/math_neon/math_neon.h>
#include <libraries/Gui/Gui.h>
#include <libraries/GuiController/GuiController.h>
#include "DirectConvolver.h"
#include "FFTConvolver.h"
#include "ZLConvolver.h"

#include <vector>
#include <cmath>
#include <cstring>
#include <chrono>

#define PLAYBACK
#define MULTICHANNEL

#ifdef PLAYBACK
#include <libraries/AudioFile/AudioFile.h>
std::vector<float> gPlayer;
size_t gReadPtr;
std::string gAudioFilename = "audio/riff.wav";
#endif // PLAYBACK

// Setup for the impulse responses
std::vector<std::string> gImpulseFilenames = {
	//"audio/large_room.wav",
	"audio/drum_room.wav",
	"audio/studio.wav",
	//"audio/room.wav",
	//"audio/plate.wav",
	//"audio/church.wav",
};

// zero-latency convolvers
std::vector<ZLConvolver> gConvolvers;

// Browser-based GUI to adjust parameters
Gui gGui;
GuiController gGuiController;

unsigned int gRoomSlider;
unsigned int gMaxBlocksSlider;
unsigned int gSparsitySlider;
unsigned int gWetSlider;
unsigned int gDrySlider;
unsigned int gTanhSlider;
unsigned int gInGainSlider;
unsigned int gOutGainSlider;

/* variables for speed testing
int k = 0;
int blockSize = 1024;
std::vector<float> h;
std::vector<float> gInputBuffer, gOutputBuffer;
std::vector<std::chrono::microseconds> timings;
int timingsCount = 0;
int totalTimings = 100;

FFTConvolver fftConvolver;
DirectConvolver directConvolver;
*/

size_t gNumChannels;

bool setup(BelaContext *context, void *userData)
{
#ifdef PLAYBACK
	// Load the audio file
	gPlayer = AudioFileUtilities::loadMono(gAudioFilename);
	if (!gPlayer.size())
	{
		fprintf(stderr, "Error loading audio file '%s'\n", gAudioFilename.c_str());
		return false;
	}

	// Print some useful info
	printf("Loaded the audio file '%s' with %d frames (%.1f seconds)\n",
			  gAudioFilename.c_str(), gPlayer.size(),
			  gPlayer.size() / context->audioSampleRate);
	gNumChannels = context->audioOutChannels;
#else // PLAYBACK
	gNumChannels = std::min(context->audioInChannels, context->audioOutChannels);
#endif // PLAYBACK
#ifdef MULTICHANNEL
	if(gImpulseFilenames.size() < gNumChannels)
	{
		fprintf(stderr, "You need as many IRs as you have channels\n");
		return false;
	}
#endif // MULTICHANNEL

	// Set up the GUI
	gGui.setup(context->projectName);

	// and attach to it
	gGuiController.setup(&gGui, "Controls");

	// Arguments: name, default value, minimum, maximum, increment
	// store the return value to read from the slider later on
	gRoomSlider = gGuiController.addSlider("Room", 0.0, 0.0, gImpulseFilenames.size(), 1.0);
	gMaxBlocksSlider = gGuiController.addSlider("Max blocks", 30.0, 0.0, 30.0, 1.0);
	gSparsitySlider = gGuiController.addSlider("Sparsity (%)", 0.0, 0.0, 1.0, 0.1);

	gTanhSlider = gGuiController.addSlider("Tanh (on/off)", 0.0, 0.0, 1.0, 1.0);
	gWetSlider = gGuiController.addSlider("Wet", 0.7, 0.0, 1.0, 0.01);
	gDrySlider = gGuiController.addSlider("Dry", 0.0, 0.0, 1.0, 0.01);
	gInGainSlider = gGuiController.addSlider("In gain (dB)", 0.0, -12.0, 12.0, 0.1);
	gOutGainSlider = gGuiController.addSlider("Out gain (dB)", 0.0, -12.0, 12.0, 0.1);

	// setup/configure the zero-latency convolvers
	// preallocate to avoid
	// surprises when taking addresses of the elements
	gConvolvers.reserve(gImpulseFilenames.size());
	for(size_t n = 0; n < gImpulseFilenames.size(); ++n)
		gConvolvers.emplace_back(context->audioFrames, context->audioSampleRate, gImpulseFilenames[n],
			context->audioSampleRate * 8 // maximum IR length
		);

	/* // convolvers for speed testing
	for (int n = 0; n < blockSize; n++)
		h.push_back(0.0);
		
	gInputBuffer.resize(131072);
	gOutputBuffer.resize(131072);
	timings.resize(totalTimings);

	directConvolver.setup(h, k, gInputBuffer, gOutputBuffer);
	fftConvolver.setup(blockSize *2, h, k, gInputBuffer, gOutputBuffer);
	*/

	return true;
}

void render(BelaContext *context, void *userData)
{
	// Access the sliders specifying the index we obtained when creating then
	int room = (int)gGuiController.getSliderValue(gRoomSlider);
	int maxBlocks = (int)gGuiController.getSliderValue(gMaxBlocksSlider);
	float sparsity = gGuiController.getSliderValue(gSparsitySlider);

	// access other sliders which do not effect state
	float nl = gGuiController.getSliderValue(gTanhSlider);
	float wet = gGuiController.getSliderValue(gWetSlider);
	float dry = gGuiController.getSliderValue(gDrySlider);
	float inGain = gGuiController.getSliderValue(gInGainSlider);
	float outGain = gGuiController.getSliderValue(gOutGainSlider);
	float inGainLinear = powf(10, inGain / 20);
	float outGainLinear = powf(10, outGain / 20);

	for (unsigned int n = 0; n < context->audioFrames; n++)
	{
		float out;
		for(unsigned int c = 0; c < gNumChannels; ++c)
		{
#ifdef PLAYBACK
			float in = gPlayer[gReadPtr] * inGainLinear;
#else
			float in = audioRead(context, n, c);
#endif // PLAYBACK
#ifdef MULTICHANNEL
			out = gConvolvers[c].process(in, maxBlocks, sparsity);
#else // MULTICHANNEL
			if(0 == c)
				out = gConvolvers[room].process(in, maxBlocks, sparsity);
#endif // MULTICHANNEL
			// wet dry mix
			out = out * wet + in * dry;
			// scale the output mix
			out = out * outGainLinear;
			// apply nonlinearity
			if(nl)
				out = tanhf_neon(out);
			audioWrite(context, n, c, out);
		}
#ifdef PLAYBACK
		if(++gReadPtr >= gPlayer.size())
			gReadPtr = 0;
#endif // PLAYBACK
	}

	/* // compute timings (ignore)
    auto start = std::chrono::high_resolution_clock::now();
    
    //for (int n = 0; n < blockSize; n++)
    //	directConvolver.process(0);
    
    fftConvolver.queue(0, false);
    fftConvolver.process();
    
    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
    timings[timingsCount++] = duration;
    
    // compute average timings
    if (timingsCount == totalTimings){
    	float avgDuration = 0;
	    for (int n = 0; n < timings.size(); n++)
	    	avgDuration += timings[n].count();
	    rt_printf("audioFrames: %0d  blockSize: %d  Avg. %0.3f us\n", context->audioFrames, blockSize, (avgDuration/totalTimings));
	    std::exit(0);
    }
    */
}

void cleanup(BelaContext *context, void *userData)
{
}
