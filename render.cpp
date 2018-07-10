#include <Bela.h>
#include <sndfile.h>
#include <resample.h>
#include <string>
#include <cmath>

#define V_FACTOR 8 // maximim varispeed factor
#define REC_BUFFER_LEN 2048
#define READ_BUFFER_LEN 8192
#define PLAY_BUFFER_LEN 16384
#define REC_RESAMP_BUFFER_LEN (V_FACTOR * REC_BUFFER_LEN)
#define PLAY_RESAMP_BUFFER_LEN (V_FACTOR * PLAY_BUFFER_LEN)

using namespace std;

// Filename to record to
string gRecordTrackName = "record.wav";

string gPlayTrackName = "play.wav";

bool gRecBufferWriting = false; // know when the buffer has written to disk
bool gPlayBufferWriting = false; // know when the buffer has written to disk

bool gRecActiveBuffer = false;
bool gPlayActiveBuffer = false;

int gReadFrames;

int recSRCerr;
int playSRCerr;

float gReadBuffer[READ_BUFFER_LEN]; // this is where the file will be read to
float gPlayBuffer[2][PLAY_BUFFER_LEN]; // double buffer, one for playing, the other for resampling from the read buffer
float gRecBuffer[2][REC_BUFFER_LEN]; // double buffer, one for recording, the other for resampling and writing to disk
float gRecResampBuffer[REC_RESAMP_BUFFER_LEN]; // buffer for storing the output of the resample function before writing to disk
float gPlayResampBuffer[REC_RESAMP_BUFFER_LEN]; // buffer for storing the output of the resample function before sending to DAC
double gResampRatio = 2.00;

static SRC_STATE *recSRC = NULL;
static SRC_STATE *playSRC = NULL;

int gRecReadPtr = REC_BUFFER_LEN;
int gPlayReadPtr = PLAY_BUFFER_LEN;

AuxiliaryTask gFillRecBufferTask;
AuxiliaryTask gFillPlayBufferTask;

SNDFILE *recsndfile ; // File we'll record to
SF_INFO recsfinfo ; // Store info about the sound file

SNDFILE *playsndfile ; // File we'll playback from
SF_INFO playsfinfo ;

int readAudio(SNDFILE *sndfile, float *buffer, int samples){
	return sf_read_float(sndfile, buffer, samples);
}

int writeAudio(SNDFILE *sndfile, float *buffer, int samples){
	sf_count_t wroteSamples = sf_write_float(sndfile, buffer, samples);
	int err = sf_error(sndfile);
	if (err) {
		rt_printf("write generated error %s : \n", sf_error_number(err));
	}
	return wroteSamples;
}

// record sample rate converter source callback
long recSRCCallback(void *cb_data, float **data) {
	int readframes = REC_BUFFER_LEN;
	*data = gRecBuffer[!gRecActiveBuffer];
	return readframes;
}

//playback sample rate converter source callback
long playSRCCallback(void *cb_data, float **data) {
	
	int readFrames = (round(float(READ_BUFFER_LEN) / gResampRatio));
	// rt_printf("\nReadFrames = %i",readFrames);
	readAudio(playsndfile, gReadBuffer,(readFrames));
	
	*data = gReadBuffer;
	return readFrames;
	
}

void fillRecBuffer(void*) {
	gRecBufferWriting = true;
    
    int recResampleFrames = ((float) REC_BUFFER_LEN * gResampRatio);
    int writeFrames = src_callback_read(recSRC, gResampRatio, recResampleFrames, gRecResampBuffer);
    // rt_printf("Should have resampled %i frames\n", resampleFrames);
    // rt_printf("Resampled to %i frames\n", writeFrames);xor
    // rt_printf("SRC returned error: %i\n",SRCerr);
    //writeFrames = REC_BUFFER_LEN;
   	writeAudio(recsndfile, gRecResampBuffer, writeFrames);
}

void  fillPlayBuffer(void*) {
	gPlayBufferWriting = true;
	int playResampleFrames = ((float) PLAY_BUFFER_LEN / gResampRatio);
	gReadFrames = src_callback_read(playSRC, 1/gResampRatio, playResampleFrames, gPlayBuffer[!gPlayActiveBuffer]);
}



bool setup(BelaContext *context, void *userData)
{
	// Initialise auxiliary tasks
	if((gFillRecBufferTask = Bela_createAuxiliaryTask(&fillRecBuffer, 90, "fill-record-buffer")) == 0)
		return false;
		
	if((gFillPlayBufferTask = Bela_createAuxiliaryTask(&fillPlayBuffer, 90, "fill-play-buffer")) == 0)
		return false;
		
	// Initialise record sample rate converter
	
	recSRC = src_callback_new(recSRCCallback, SRC_LINEAR, 1, &recSRCerr, NULL);
	
	playSRC = src_callback_new(playSRCCallback, SRC_LINEAR, 1, &playSRCerr, NULL);

		
	// Open the record sound file
	recsfinfo.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT; // Specify the write format, see sndfile.h for more info.
	recsfinfo.samplerate = context->audioSampleRate ; // Use the Bela context to set sample rate
	recsfinfo.channels = context->audioInChannels ; // Use the Bela context to set number of channels
	if (!(recsndfile = sf_open (gRecordTrackName.c_str(), SFM_WRITE, &recsfinfo))) {
		rt_printf("Couldn't open file %s : %s\n",gRecordTrackName.c_str(),sf_strerror(recsndfile));
	}
	
	// Locate the start of the record file
	sf_seek(recsndfile, 0, SEEK_SET);
	
	// Open the playback sound file
    
	playsfinfo.format = 0;
	if (!(playsndfile = sf_open (gPlayTrackName.c_str(), SFM_READ, &playsfinfo))) {
		rt_printf("Couldn't open file %s : %s\n",gPlayTrackName.c_str(),sf_strerror(recsndfile));
	}
	
	// Locate the start of the record file
	sf_seek(playsndfile, 0, SEEK_SET);
	
	
	// We should see a file with the correct number of channels and zero frames
	rt_printf("Record file contains %i channel(s)\n", recsfinfo.channels);
	
	// We should see a file with the correct number of channels and zero frames
	rt_printf("Play file contains %i channel(s)\n", playsfinfo.channels);
	
	return true;
}

void render(BelaContext *context, void *userData)
{
	for(unsigned int n = 0; n < context->audioFrames; n++) {
		//record audio interleaved in gRecBuffer
    	for(unsigned int channel = 0; channel < context->audioInChannels; channel++) {
    		// when gRecBuffer is full, switch to other buffer, write audio to disk using the callback
    		if(++gRecReadPtr >= REC_BUFFER_LEN) {
	            if(!gRecBufferWriting)
	                rt_printf("Couldn't write buffer in time -- try increasing record buffer size\n");
	            Bela_scheduleAuxiliaryTask(gFillRecBufferTask);
	            // switch buffer
	            gRecActiveBuffer = !gRecActiveBuffer;
	            // clear the buffer writing flag
	            gRecBufferWriting = false;
	            gRecReadPtr = 0;
        	}
        	if(++gPlayReadPtr >= gReadFrames) {
        		if(!gPlayBufferWriting)
                	rt_printf("Couldn't load buffer in time -- try increasing play buffer size\n");
	            
	            Bela_scheduleAuxiliaryTask(gFillPlayBufferTask);
	            gPlayActiveBuffer = !gPlayActiveBuffer;
	            gPlayBufferWriting = false;
	            gPlayReadPtr = 0;
        	}
        	// store the sample from the audioRead buffer in the active buffer
    		gRecBuffer[gRecActiveBuffer][gRecReadPtr] = audioRead(context, n, channel);
    		// write the sample to the DAC
    		// float out = gPlayResampBuffer[gPlayReadPtr];
    		audioWrite(context, n, channel, gPlayBuffer[gPlayActiveBuffer][gPlayReadPtr]);
    	}
    }
}

void cleanup(BelaContext *context, void *userData)
{
	rt_printf("Closing sound files...");
	sf_close (recsndfile);
}