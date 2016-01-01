//	this is free public source code, no restrictions, no warranties
//
//	beos soundblaster device driver v1.1
//	modified 4/2002 by diamond of the musclesoft crew
//	www.musclesoft.de/diamond
//
//	sources & references:
//	[1 a]	BE ENGINEERING INSIGHTS: Writing A Sound Card Driver (By Marc Ferguson)
//	[1 b]	DEVELOPERS' WORKSHOP: Updating An Old Sound Card Driver (By Marc Ferguson)
//	[2]	Device Drivers in the Be Os - Old Sound Card Device Driver (ALPHA) (The Communal Be Documentation Site)
//	[3]	The Be Book 5
//	[4]	BeOS Device Driver Development Hints and Information
//
//	version history
//	1.1	write sb-mixer registers; scan legacy devices; bugfix:ignore failing ports
//	1.0p	experimental: use pci module instead of isa module

#include <alloc.h>
#include <KernelExport.h>
#include <config_manager.h>
#include <driver_settings.h>
#include <ISA.h>
#include <PCI.h>

// use pci instead of isa:
/*
#define isa_module_info pci_module_info
#undef B_ISA_MODULE_NAME
#define B_ISA_MODULE_NAME B_PCI_MODULE_NAME
*/

struct sound_setup { // [1 a]
	struct channel {
		enum adc_source { line, cd, mic, loopback } adc_source;
		char adc_gain;
		char mic_gain_enable;
		char cd_mix_gain;
		char cd_mix_mute;
		char aux2_mix_gain;
		char aux2_mix_mute;
		char line_mix_gain;
		char line_mix_mute;
		char dac_attn;
		char dac_mute;
	} left, right;
	enum sample_rate {
		kHz_8_0 = 0,
		kHz_5_51,
		kHz_16_0,
		kHz_11_025,
		kHz_27_42,
		kHz_18_9,
		kHz_32_0,
		kHz_22_05,
		kHz_37_8 = 9,
		kHz_44_1 = 11,
		kHz_48_0,
		kHz_33_075,
		kHz_9_6,
		kHz_6_62
	} sample_rate;
	enum {} playback_format;
	enum {} capture_format;
	char dither_enable;
	char mic_attn;
	char mic_enable;
	char output_boost;
	char highpass_enable;
	char mono_gain;
	char mono_mute;
};

struct Samplerate{unsigned hz; enum sound_setup::sample_rate id;};
static const Samplerate samplerate_choice[]=	//commented out rates didn't work on my system (reason unknown)
{
//	{5510,sound_setup::kHz_5_51},
//	{6620,sound_setup::kHz_6_62},
	{8000,sound_setup::kHz_8_0},
//	{9600,sound_setup::kHz_9_6},
	{11025,sound_setup::kHz_11_025},
	{16000,sound_setup::kHz_16_0},
//	{18900,sound_setup::kHz_18_9},
	{22050,sound_setup::kHz_22_05},
//	{27420,sound_setup::kHz_27_42},
	{32000,sound_setup::kHz_32_0},
//	{33075,sound_setup::kHz_33_075},
//	{37800,sound_setup::kHz_37_8},
	{44100,sound_setup::kHz_44_1},
	{48000,sound_setup::kHz_48_0},
};

#define DRIVER_NAME "sbcompat"

static const char * available_device_names[] = {
	//the current implementation uses (last_char-'1') as id, so this list is complete
	"audio/old/"DRIVER_NAME"/1",
	"audio/old/"DRIVER_NAME"/2",
	"audio/old/"DRIVER_NAME"/3",
	"audio/old/"DRIVER_NAME"/4",
	"audio/old/"DRIVER_NAME"/5",
	"audio/old/"DRIVER_NAME"/6",
	"audio/old/"DRIVER_NAME"/7",
	"audio/old/"DRIVER_NAME"/8",
	"audio/old/"DRIVER_NAME"/9",
};
static int const MAX_DEVICES = sizeof(available_device_names) / sizeof(*available_device_names);
static const char* device_names[MAX_DEVICES + 1] = {NULL}; //+ NULL terminator

static Samplerate rate_in; // the sample rate of the system delivered audio data
#define RATE_IN (rate_in.hz)
static unsigned const DEFAULT_RATE = 11025;
static unsigned RATE_OUT = DEFAULT_RATE; // the output sample rate
static unsigned const ONE_SECOND = 1000000; // in usec (bigtime_t units)
static int const BUFFER_LEN =
	0x8000; // output sample (8 bit mono @ output rate) buffer size, 0x8000 can hold 0.68s@48k
static bool debug = false; // noisy function calls
static bool skip_reset = false; // dont use sb read registers
static bool no_mixer = false; // dont use sb mixer registers
static bool honour_system_rate = false; // ignore restrictions (e. g. <= 22050hz) of sample rate
static int devices = 0; // number of configured devices found
static isa_module_info* bus; // needed for io port interface

void best_input_rate() // find most convenient input sample rate
{
	for (unsigned i = 0; i < sizeof(samplerate_choice) / sizeof(*samplerate_choice); i++) {
		rate_in = samplerate_choice[i];
		if (RATE_IN >= RATE_OUT) break;
	}
}

static class Device
{
public:
	bool available; // device is ok
	unsigned port; // sb base port
	unsigned command_port; // sb data out port
	sound_setup setup; // current parameters (mixer etc) mirror

	int open_count; // number of clients currently using device

	struct {
		unsigned char* buffer; // circular buffer where write==read means emptyness
		unsigned write; // empty section starts here (inclusive)
		unsigned read; // filled section starts here (inclusive)
		bigtime_t clock; // see [1 b]
		unsigned recovery; // number of silent samples to be played (when buffer underrun)
	} sample;
	unsigned remainder; // contains intermediate results for conversion from rate_in to rate_out
	sem_id sem_playback; // client delivered
	spinlock lock; // general purpose
	class Timer : public timer
	{
	public:
		class Device* device; // this pointer for timer interrupt
	} dev_timer;

	static int32 timer_hook(timer*); // called periodically (at output sample rate)
	static int32 recovery_timer_hook(timer*); // called once for each recovery

	void soundblaster_reset()
	{
		if (skip_reset)
			available = true;
		else // reset procedure
		{
			bus->write_io_8(port + 0x6, 1);
			snooze(1000);
			bus->write_io_8(port + 0x6, 0);
			snooze(1000);

			cpu_status former = disable_interrupts();
			acquire_spinlock(&lock);
			{
				bigtime_t timeout = system_time() + 25; // max allowed interrupt disable time: 50
				do {
					if ((bus->read_io_8(port + 0xe) & 0x80) &&
						(bus->read_io_8(port + 0xa) == 0xaa)) {
						available = true;
					}
				} while (!available && system_time() < timeout);
			}
			release_spinlock(&lock);
			restore_interrupts(former);
		}
	}

	inline void soundblaster_write_data(unsigned char s) // should be called with lock held
	{
		while (bus->read_io_8(command_port) & 0x80)
			;
		bus->write_io_8(command_port, s);
	}

	inline void soundblaster_write_sample(unsigned char s) // should be called with lock held
	{
		soundblaster_write_data(0x10); // this is "it", command 0x10: write 8 bit sample to dac
		soundblaster_write_data(s);
	}

	inline void soundblaster_write_mixer(unsigned char index, unsigned char data)
	{
		if (no_mixer)
			return;
		cpu_status former = disable_interrupts();
		acquire_spinlock(&lock);
		{
			bus->write_io_8(port + 0x4, index); // mixer index
			bus->write_io_8(port + 0x5, data); // mixer data
		}
		release_spinlock(&lock);
		restore_interrupts(former);
	}

	inline void write_mixer()
	{
		enum {
			MIXER_RESET = 0x00,
			CT1335_MASTER = 0x02, // mono
			CT1345_VOICE = 0x04,
			CT1345_MASTER = 0x22,
			CT1345_CD = 0x28,
			CT1345_LINE = 0x2e,
			CT1745_OUTPUT_SWITCH = 0x3c,
			CT1745_TREBLE_L = 0x44,
			CT1745_TREBLE_R = 0x45,
			CT1745_BASS_L = 0x46,
			CT1745_BASS_R = 0x47,
		};
		struct {
			unsigned char reg, val;
		} const defaults[] = {
			{CT1745_TREBLE_L, 4 << 4},
			{CT1745_TREBLE_R, 4 << 4},
			{0x48, 0xff}, // correct ess1869 bugs
			{MIXER_RESET, 0xff}, // reset
			{CT1345_VOICE, 12 | (12 << 4)}, // default voice level
			//{CT1745_OUTPUT_SWITCH,0x1f},
			//{0x3d,0x15},
			//{0x3e,0x0b},
			//{CT1745_TREBLE_L,8<<4},
			//{CT1745_TREBLE_R,8<<4},
			//{CT1745_BASS_L,8<<4},
			//{CT1745_BASS_R,8<<4},
		};
		// defaults:
		for (unsigned d = 0; d < sizeof(defaults) / sizeof(*defaults); d++) {
			soundblaster_write_mixer(defaults[d].reg, defaults[d].val);
		}
		// mixer:
		unsigned char left = 255 - (setup.left.dac_attn * 4); // input is 0...63
		unsigned char right = 255 - (setup.right.dac_attn * 4);
		soundblaster_write_mixer(CT1335_MASTER, ((left + right) >> 5) & 0xe);
		soundblaster_write_mixer(CT1345_MASTER, ((left & 0xf0) | (right >> 4)) & 0xee);
		// line:
		left = 255 - (setup.left.line_mix_gain * 8); // input is 0...31
		right = 255 - (setup.right.line_mix_gain * 8);
		soundblaster_write_mixer(CT1345_LINE, ((left & 0xf0) | (right >> 4)) & 0xee);
		// cd:
		left = 255 - (setup.left.cd_mix_gain * 8); // input is 0...31
		right = 255 - (setup.right.cd_mix_gain * 8);
		soundblaster_write_mixer(CT1345_CD, ((left & 0xf0) | (right >> 4)) & 0xee);
	}

	inline void play_sample(unsigned char s) // calls write_sample safely
	{
		cpu_status former = disable_interrupts();
		acquire_spinlock(&lock);
		{
			soundblaster_write_sample(s);
		}
		release_spinlock(&lock);
		restore_interrupts(former);
	}

	void test(unsigned frequency, unsigned duration) // make a funny noise
	{
		if (debug)
			while (duration--) play_sample((frequency * duration & 0x3f) + 0x60), snooze(100);
	}

	inline unsigned queue_length() // get current size of filled area in buffer
	{
		int result;

		cpu_status former = disable_interrupts();
		acquire_spinlock(&lock);
		{
			result = sample.write - sample.read;
		}
		release_spinlock(&lock);
		restore_interrupts(former);

		if (result < 0)
			result += BUFFER_LEN; // area wraps around
		return result;
	}

	inline bool
	queue_sample(unsigned char s) // put a sample into output buffer; out: false if full on entry
	{
		bool success;

		cpu_status former = disable_interrupts();
		acquire_spinlock(&lock);
		{
			success = ((sample.write + 1) % BUFFER_LEN != sample.read);
			if (success) {
				sample.buffer[sample.write++] = s;
				sample.write %= BUFFER_LEN;
			}
		}
		release_spinlock(&lock);
		restore_interrupts(former);

		return success;
	}

	void stop_timer() { cancel_timer(&dev_timer); }
	status_t start_timer()
	{
		stop_timer(); // just in case it's already running, won't hurt if not
		status_t result;
		cpu_status former = disable_interrupts();
		acquire_spinlock(&lock);
		{
			sample.recovery = 0;
			sample.clock = system_time(); // clean start
			result = add_timer(&dev_timer, timer_hook, ONE_SECOND / RATE_OUT, B_PERIODIC_TIMER);
		}
		release_spinlock(&lock);
		restore_interrupts(former);
		return result;
	}
	status_t start_recovery_timer() // should not be called before start_timer()
	{
		stop_timer(); // just in case it's already running, won't hurt if not
		status_t result;
		cpu_status former = disable_interrupts();
		acquire_spinlock(&lock);
		{
			sample.clock += (ONE_SECOND / RATE_OUT * sample.recovery + 1) / 2; // first half
			result = add_timer(&dev_timer, recovery_timer_hook,
							   ONE_SECOND / RATE_OUT * sample.recovery, B_ONE_SHOT_RELATIVE_TIMER);
		}
		release_spinlock(&lock);
		restore_interrupts(former);
		return result;
	}
	status_t init() // initializer - must be first function called, expects 'port' to be set
	{
		available = false;
		dev_timer.device = this;
		command_port = port + 0xc;
		lock = 0;
		remainder = 0;
		open_count = 0;
		sem_playback = B_OK - 1;
		memset(&sample, 0, sizeof(sample));
		memset(&setup, 0, sizeof(setup));

		soundblaster_reset(); // will set 'available' on success
		if (!available) return EIO;

		sample.buffer = (unsigned char*)malloc(BUFFER_LEN);
		if (!sample.buffer) return ENOSYS;

		return B_OK;
	}
	void uninit()
	{
		stop_timer(); // will not hurt
		if (sample.buffer) free(sample.buffer);
	}
	status_t open()
	{
		if (!available) return EINVAL;
		if (!open_count++) // execute for first client
		{
			{
				for (int i = 1; i <= 20; test(i++, 100))
					;
			}
			best_input_rate(); // set rate_in
			return start_timer(); // go!
		}
		return B_OK;
	}
	status_t close()
	{
		// release_sem(sem_playback);
		if (!--open_count) // execute if no client left
		{
			stop_timer();
			sample.read = sample.write = 0; // empty sample buffer
			{
				for (int i = 20; i >= 1; test(i--, 100))
					;
			}
		}
		return B_OK;
	}
} device_instance[MAX_DEVICES] = {{0}};

int32 Device::recovery_timer_hook(timer* tp)
{
	Timer& t = *(Timer*)tp;
	Device& device = *t.device;

	spinlock slock = 0;
	acquire_spinlock(&slock);
	{
		device.sample.clock += t.period / 2; // second half
	}
	release_spinlock(&slock);

	device.start_timer(); // re-activate periodic timer

	return B_HANDLED_INTERRUPT;
}

int32 Device::timer_hook(timer* tp) // called at sample rate, writes one sample (if available)
{
	Timer& t = *(Timer*)tp;
	Device& device = *t.device;

	spinlock slock = 0;
	acquire_spinlock(&slock);
	{
		unsigned read = device.sample.read;
		if (read != device.sample.write) // no underrun
		{
			device.soundblaster_write_sample(device.sample.buffer[read]);
			device.sample.read = (read + 1) % BUFFER_LEN;
			device.sample.clock += t.period; // advance one sample
		} else
			device.sample.recovery = (ONE_SECOND / 4) / t.period; // silent recovery time interval
	}
	release_spinlock(&slock);

	if (device.sample.recovery) device.start_recovery_timer(); // deactivate periodic timer

	return B_HANDLED_INTERRUPT;
}

void read_configuration()
{
	void* handle = load_driver_settings(DRIVER_NAME); //[3] driver settings api
	debug = get_driver_boolean_parameter(handle, "debug_noise", false, false);
	skip_reset = get_driver_boolean_parameter(handle, "skip_reset", false, false);
	no_mixer = get_driver_boolean_parameter(handle, "ignore_mixer", false, false);
	bool legacy = get_driver_boolean_parameter(handle, "scan_legacy_devices", false, false);
	char const* value = get_driver_parameter(handle, "samplerate", NULL, NULL);
	if (value != NULL) RATE_OUT = atoi(value);
	if (RATE_OUT <= 0) RATE_OUT = DEFAULT_RATE;
	RATE_OUT =
		ONE_SECOND / (ONE_SECOND / RATE_OUT); // find next possible rate with usec time granularity
	unload_driver_settings(handle);

	// the following (hack) is based on various public sources and the respective header files:
	config_manager_for_driver_module_info* config;
	if (get_module(B_CONFIG_MANAGER_FOR_DRIVER_MODULE_NAME, (module_info**)&config) != B_OK) return;

	device_info info;
	for (int pci = 0; pci <= 1; pci++)
		for (uint64 cookie = 0;
			 config->get_next_device_info(pci ? B_PCI_BUS : B_ISA_BUS, &cookie, &info,
				sizeof(info)) == B_OK;) { // scan isa and pci devices
			if (info.config_status != B_OK)
				continue;
			bool is_sound = (info.devtype.base == 4 && info.devtype.subtype == 1 &&
				info.devtype.interface == 0);
			bool is_legacy = (info.devtype.base == 0 && info.devtype.subtype == 0 &&
				info.devtype.interface == 0);
			if (!is_sound && !(legacy && is_legacy))
				continue; // no "multimedia device (sound)" or "legacy"
			int size = config->get_size_of_current_configuration_for(cookie);
			if (size < 0)
				continue;
			device_configuration* device = (device_configuration*)malloc(size);
			if (device == NULL)
				continue;
			if (config->get_current_configuration_for(cookie, device, size) == B_OK) {
				int ports = config->count_resource_descriptors_of_type(device, B_IO_PORT_RESOURCE);
				for (int n = 0; n < ports && devices < MAX_DEVICES; n++) {
					resource_descriptor resource;
					if (config->get_nth_resource_descriptor_of_type(
							device, n, B_IO_PORT_RESOURCE, &resource, sizeof(resource)) != B_OK)
						continue;
					unsigned port = resource.d.r.minbase;
					if ((port & ~0xf0) != 0x200) continue; // port out of soundblaster dsp range
					device_instance[devices++].port = port; // got one!
				}
			}
			free(device);
		}
	put_module(B_CONFIG_MANAGER_FOR_DRIVER_MODULE_NAME);
}

// start of driver interface section //

#include <Drivers.h>
#include <KernelExport.h>
#include <MediaDefs.h>
#include <memory.h>

int32 api_version = B_CUR_DRIVER_API_VERSION;

status_t init_driver(void) // first function called
{
	status_t result = B_OK;

	read_configuration(); // read device configuration, required rate and options
	if (!devices) result = ENODEV;

	if (result == B_OK) result = get_module(B_ISA_MODULE_NAME, (module_info**)&bus);
	int pos = 0;
	for (int d = 0; d < devices; d++) {
		result = device_instance[d].init(); // init found devices
		if (result == B_OK) device_names[pos++] = available_device_names[d];
	}
	device_names[pos] = NULL;

	if (!pos) uninit_driver();
	return pos ? B_OK : !B_OK;
}
void uninit_driver(void)
{
	for (int i = devices - 1; i >= 0; i--) device_instance[i].uninit(); // uninit found devices
	put_module(B_ISA_MODULE_NAME);
}
const char** publish_devices(void)
{
	return device_names;
}

status_t dev_open_hook(const char* name, uint32 flags, void** cookie)
{
	int id = -1;
	while (*name)
		id = (*name++) - '1'; // identify by last character (/dev/.../1 /dev/.../2 and so on)
	if (id < 0 || id >= devices) return EINVAL; // invalid name
	if (!device_instance[id].available) return B_IO_ERROR; // not available

	*cookie = &device_instance[id]; // static functions will see a 'this' pointer in the cookie
	return device_instance[id].open();
}
status_t dev_close_hook(void* cookie)
{
	return ((Device*)cookie)->close();
}
status_t dev_free_hook(void* cookie)
{
	return B_OK;
}
status_t dev_control_hook(void* cookie, uint32 op, void* data, size_t len)
{
	Device& device = *(Device*)cookie;
	enum //[1 b]
	{ SOUND_GET_PARAMS = B_DEVICE_OP_CODES_END,
	  SOUND_SET_PARAMS,
	  SOUND_SET_PLAYBACK_COMPLETION_SEM,
	  SOUND_SET_CAPTURE_COMPLETION_SEM,
	  SOUND_RESERVED_1,
	  SOUND_RESERVED_2,
	  SOUND_DEBUG_ON,
	  SOUND_DEBUG_OFF,
	  SOUND_WRITE_BUFFER,
	  SOUND_READ_BUFFER,
	  SOUND_LOCK_FOR_DMA,
	  SOUND_SET_CAPTURE_PREFERRED_BUF_SIZE,
	  SOUND_SET_PLAYBACK_PREFERRED_BUF_SIZE,
	  SOUND_GET_CAPTURE_PREFERRED_BUF_SIZE,
	  SOUND_GET_PLAYBACK_PREFERRED_BUF_SIZE };
	typedef struct audio_buffer_header //[1 b]
	{
		int32 buffer_number;
		int32 subscriber_count;
		bigtime_t time;
		int32 reserved_1;
		int32 reserved_2;
		bigtime_t sample_clock;
	} audio_buffer_header;

	switch (op) // see [1 a] and [1 b]
	{
	case SOUND_SET_PLAYBACK_COMPLETION_SEM: {
		device.sem_playback = *(sem_id*)data;
	} break;
	case SOUND_WRITE_BUFFER: // ioctl write, as used by the media server
	{
		audio_buffer_header* header = (audio_buffer_header*)data;
		int32 bytes_of_data = header->reserved_1 - sizeof(*header); //[1 b]
		int16* addr_of_data = (int16*)(header + 1);					//[1 b]

		unsigned samples_in = bytes_of_data / 4; // source is 16 bit, 2 channels
		unsigned samples_out = 0;

		unsigned remainder = device.remainder;
		for (unsigned in = 0; in < samples_in; samples_out++) // read whole input buffer
		{
			unsigned char s = ((addr_of_data[in * 2] + addr_of_data[in * 2 + 1]) >> 9) +
							  128; // pick next output sample
			while (!device.queue_sample(s))
				snooze(ONE_SECOND / RATE_OUT * device.queue_length() / 2 +
					   1); // sleep if buffer overrun
			in += (RATE_IN + remainder) / RATE_OUT;
			remainder = (RATE_IN + remainder) % RATE_OUT;
		}
		device.remainder = remainder;

		int samples_to_go =
			(int)device.queue_length() -
			(int)samples_out; // distance from first queued sample to first sample of current buffer
		header->time = system_time() + ONE_SECOND / RATE_OUT * samples_to_go; // see [1 a]
		header->sample_clock =
			device.sample.clock + ONE_SECOND / RATE_OUT * samples_to_go; // see [1 b]

		release_sem(device.sem_playback); // unlock input buffer

		while (device.queue_length() > RATE_OUT / 10 + 1)
			snooze(ONE_SECOND / 100); // sleep if latency too high
	} break;
	case SOUND_SET_PARAMS: {
		device.test(1, 200);
		sound_setup& setup = *(sound_setup*)data;

		// mixer:
		device.setup = setup;
		device.write_mixer();
		// samplerate:
		for (unsigned i = 0; i < sizeof(samplerate_choice) / sizeof(*samplerate_choice); i++) {
			if (honour_system_rate && setup.sample_rate == samplerate_choice[i].id) {
				rate_in = samplerate_choice[i]; // use system's favorite input sample rate
				return B_OK;
			}
		}
		return B_BAD_VALUE; // unknown rate id
	} break;
	case SOUND_GET_PARAMS: {
		device.test(4, 200);
		sound_setup& setup = *(sound_setup*)data;
		device.setup.sample_rate = rate_in.id; // set required input sample rate
		setup = device.setup;
	} break;
	case SOUND_GET_PLAYBACK_PREFERRED_BUF_SIZE: {
		device.test(3, 300);
		return B_BAD_VALUE;
	}
	case SOUND_SET_PLAYBACK_PREFERRED_BUF_SIZE: {
		device.test(2, 300);
	}
	default: {
		device.test(8, 500);
		return B_BAD_VALUE;
	}
	};
	return B_OK;
}

status_t dev_read_hook(void* cookie, off_t position, void* data,
					   size_t* len) // io read from device, accessible from the shell
{
	char t[] = "# 0\t& 000\t@     0Hz\t~     0Hz\n";
		// output some status info
	int i, n, size = sizeof(t) - 1;

	n = RATE_IN;
	size -= 4;
	for (i = size; n && i >= 0; i--, n /= 10) t[i] = n % 10 + '0';
	n = RATE_OUT;
	size -= 10;
	for (i = size; n && i >= 0; i--, n /= 10) t[i] = n % 10 + '0';
	n = ((Device*)cookie)->port;
	size -= 8;
	for (i = size; n && i >= 0; i--, n /= 16)
		t[i] = (n % 16 > 9) ? (n % 16 - 10 + 'a') : (n % 16 + '0');
	n = ((Device*)cookie - device_instance) + 1;
	size -= 6;
	for (i = size; n && i >= 0; i--, n /= 10) t[i] = n % 10 + '0';

	static int pos = 0; // unsafe, outputs from more than one device will 'mix'
	if (t[pos]) {
		*(char*)data = t[pos++];
		*len = 1;
	} else {
		pos = *len = 0;
	}
	return B_OK;
}

status_t dev_write_hook(void* cookie, off_t position, const void* data,
						size_t* len) // io write to device, accessible from the shell
{
	char const* p = (char const*)data;
	char const* pe = p + *len;
	for (; p < pe; p++) { // write 8 bit samples at output rate
		while (!((Device*)cookie)->queue_sample(*p))
			;
	}
	return B_OK;
}

device_hooks* find_device(const char* name)
{
	static device_hooks device = {
		dev_open_hook,	dev_close_hook,	dev_free_hook,	dev_control_hook,
		dev_read_hook,	dev_write_hook,	NULL,	NULL,	NULL,	NULL
	};
	return &device; // all devices use the same hooks
}
