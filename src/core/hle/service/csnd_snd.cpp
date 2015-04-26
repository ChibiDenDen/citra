// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/hle/kernel/shared_memory.h"
#include "core/hle/kernel/mutex.h"
#include "core/hw/hw.h"

#include "core/hle/hle.h"
#include "core/hle/service/csnd_snd.h"
#include <future>
#include <portaudio.h>

#define SAMPLE_RATE (44100)
#define CURRENT_CHANNEL (&state.Channels[state.current_channel])


static std::future<void>* Async(std::function<void()> func)
{
	return new auto(std::async(std::launch::async, func));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Namespace CSND_SND
namespace CSND_SND {

	enum LerpMode {
		Enable,
		Disable
	};

	enum RepeatMode {
		Manual,
		Normal,
		OneShot,
		LoopConstSize
	};

	enum Encoding {
		PCM8, 
		PCM16, 
		IMA_ADPCM,
		PSG
	};

struct SoundState {
    Kernel::SharedPtr<Kernel::SharedMemory> shared_memory;
    u32 shared_memory_size;
    u32 offsets[4];
    Kernel::SharedPtr<Kernel::Mutex> mutex;


	struct ChannelState {
		u8* Data1;
		u32 Size1;
		u8* Data2;
		u32 Size2;
		bool Looping;
		bool playing;
		std::future<void>* cur_play = nullptr;
		PaStream *stream;
		u32 SampleRate;

		LerpMode lerp_mode;
		RepeatMode repeat_mode;
		Encoding encoding;

		u16 vol_left;
		u16 vol_right;

		u16 capture_vol_left;
		u16 capture_vol_right;
	};

	u32 current_channel;
	ChannelState Channels[0x1f];
}state;

static u32 ConvertSampleRate(u32 input_rate) {
	return  67027964 / input_rate;
}

static void Initialize(Service::Interface* self) {
    u32 * cmd_buff = Kernel::GetCommandBuffer();
    
    state.shared_memory_size = cmd_buff[1];
    for (auto i = 0; i < 4; i++) {
        state.offsets[i] = cmd_buff[2 + i];
    }
    
    state.shared_memory = Kernel::SharedMemory::Create("CSND-SHARED_MEM");
    auto shared_memory_handle = Kernel::g_handle_table.Create(state.shared_memory);

    state.mutex = Kernel::Mutex::Create(false, "CSND-MUTEX");
    auto mutex_handle = Kernel::g_handle_table.Create(state.mutex);

    cmd_buff[2] = 0x4000000; // constant, Handle-list header
    cmd_buff[3] = mutex_handle.ValueOr(0);
    cmd_buff[4] = shared_memory_handle.ValueOr(0);

    cmd_buff[1] = RESULT_SUCCESS.raw;

	auto err = Pa_Initialize();
	if (err != paNoError) {
		// TODO: print error
	}

	PaStreamParameters outputParameters;

	outputParameters.device = Pa_GetDefaultOutputDevice(); /* default output device */
	outputParameters.channelCount = 1;
	outputParameters.sampleFormat = paInt16;
	outputParameters.suggestedLatency = Pa_GetDeviceInfo(outputParameters.device)->defaultHighOutputLatency;
	outputParameters.hostApiSpecificStreamInfo = NULL;
	/* Open an audio I/O stream. */
	for (auto i = 0; i < 0x1f; i++) {
		err = Pa_OpenStream(&state.Channels[i].stream, nullptr, &outputParameters, SAMPLE_RATE, paFramesPerBufferUnspecified, 0, nullptr, nullptr);
		if (err != paNoError) {
			// TODO: print error
		}
	}

	state.current_channel = 0;
}

struct Command {
    u16 next_command_offset;
    u16 id;
    u32 unk;
    u32 params[6];
};

static void Shutdown(Service::Interface* self);

static void PlayChannel(SoundState::ChannelState* channel) {
	if (!channel->playing)
	{
		u8* PlayData1 = new u8[channel->Size1];
		u32 PlaySize1 = channel->Size1;
		channel->playing = true;
		std::copy(channel->Data1, channel->Data1 + channel->Size1, PlayData1);
		channel->cur_play = Async([PlayData1, PlaySize1, channel](){
			Pa_StartStream(channel->stream);

			auto size = PlaySize1;
			auto data = PlayData1;
			auto offset = 0;
			while (channel->playing)
			{
				//printf("XXXX\n");
				if (size - offset < 0x400)
				{
					Pa_WriteStream(channel->stream, data + offset, (size - offset) / 2);
					offset = 0;
					size = channel->Size2;
					data = channel->Data2;
				}
				else
				{
					Pa_WriteStream(channel->stream, data + offset, (0x400) / 2);
					offset += 0x400;
				}
			}
			Pa_StopStream(channel->stream);
			delete [] PlayData1;
		});
	}
}

static void StopChannel(SoundState::ChannelState* channel) {
	channel->playing = false;
	if (channel->cur_play != nullptr)
	{
		channel->cur_play->wait();
	}
	channel->cur_play = nullptr;
}

typedef void(*Type0CommandFunction)(u32 params[6]);
static void SetPlayState(u32 params[6]);
// temporary nullptr until implemented
static void SetPlayStateR(u32 params[6]) {
	SetPlayState(params);
    // TODO: reset state
}
static void SetPlayState(u32 params[6]) {
	bool play = params[1] == 1;	
	u32 channel = params[0];

	state.current_channel = channel;
	
	printf("channel: %d\n", params[0]);
	
	if (play) {
		PlayChannel(CURRENT_CHANNEL);
	}
	else {
		StopChannel(CURRENT_CHANNEL);
	}
}
#define SetEncoding nullptr
static void SetBlock(u32 params[6]) {
	u32 block_size = params[2];

	if (params[1] != 0)
	{
		u8* block2 = Memory::GetPointer(Memory::PhysicalToVirtualAddress(params[1]));
		auto data = block2 + 0x2c;

		state.Channels[state.current_channel].Data2 = data;
		state.Channels[state.current_channel].Size2 = block_size;
	}
	else
	{
		state.Channels[state.current_channel].Size2 = block_size;
	}
}
#define SetLooping nullptr
#define SetBit7 nullptr
#define SetInterp nullptr
#define SetDuty nullptr
#define SetTimer nullptr
#define SetVol nullptr
#define SetBlockZero nullptr
#define SetAdpcmStateZero nullptr
#define SetAdpcmState nullptr
#define SetAdpcmReload nullptr
static void SetChnRegs(u32 params[6]) {
    u32 block_size = params[5];
    
	struct Flags{
		union{
			u32 Value;
			struct{
				
				u32 Channel : 6;
				u32 Linear : 1;
				u32 pad1 : 3;
				u32 RepeatMode : 2;
				u32 Encoding : 2;
				u32 Playback : 1;
				u32 pad2 : 1;
				u32 SampleRate : 16;
			};
		};
	};

	struct TwoWords {
		union {
			u32 value;
			struct {
				u16 low_word;
				u16 high_word;
			};
		};
	};
	/* read params */
	Flags flags = { 0 };
	flags.Value = params[0];
	TwoWords channel_volume;
	channel_volume.value = params[1];
	TwoWords capture_volume; 
	capture_volume.value = params[2];
	auto block1_p_addr = params[3];
	auto block2_p_addr = params[4];
	

	state.current_channel = flags.Channel;
	auto channel = CURRENT_CHANNEL;
	
	channel->lerp_mode = (LerpMode)flags.Linear;
	channel->repeat_mode = (RepeatMode)flags.RepeatMode;
	channel->encoding = (Encoding) flags.Encoding;
	channel->SampleRate = ConvertSampleRate(flags.SampleRate);

	channel->vol_left = channel_volume.low_word;
	channel->vol_right = channel_volume.high_word;

	channel->capture_vol_left = capture_volume.low_word;
	channel->capture_vol_right = capture_volume.high_word;


	if (block1_p_addr != 0)	{
		u8* block1 = Memory::GetPointer(Memory::PhysicalToVirtualAddress(block1_p_addr));
		
		auto data = block1 + 0x2c;
		channel->Data1 = data;
		channel->Size1 = block_size;
	}

	if (block2_p_addr != 0)	{
		u8* block2 = Memory::GetPointer(Memory::PhysicalToVirtualAddress(block2_p_addr));

		auto data = block2 + 0x2c;
		channel->Data2 = data;
		channel->Size2 = block_size;
	}

	// TODO: handle playback flag
}

#define SetChnRegsPSG nullptr
#define SetChnRegsNoise nullptr
#define CapEnable nullptr
#define CapSetRepeat nullptr
#define CapSetFormat nullptr
#define CapSetBit2 nullptr
#define CapSetTimer nullptr
#define CapSetBuffer nullptr
#define SetCapRegs nullptr
#define SetDspFlags nullptr
#define UpdateInfo nullptr
static const std::unordered_map<u16, Type0CommandFunction> type0_commands = {
    { 0x0,   SetPlayStateR },
    { 0x1,   SetPlayState },
    { 0x2,   SetEncoding },
    { 0x3,   SetBlock },
    { 0x4,   SetLooping },
    { 0x5,   SetBit7 },
    { 0x6,   SetInterp },
    { 0x7,   SetDuty },
    { 0x8,   SetTimer },
    { 0x9,   SetVol },
    { 0xA,   SetBlockZero },
    { 0xB,   SetAdpcmStateZero },
    { 0xC,   SetAdpcmState },
    { 0xD,   SetAdpcmReload },
    { 0xE,   SetChnRegs },
    { 0xF,   SetChnRegsPSG },
    { 0x10,  SetChnRegsNoise },
    { 0x100, CapEnable },
    { 0x101, CapSetRepeat },
    { 0x102, CapSetFormat },
    { 0x103, CapSetBit2 },
    { 0x104, CapSetTimer },
    { 0x105, CapSetBuffer },
    { 0x106, SetCapRegs },
    { 0x200, SetDspFlags },
    { 0x300, UpdateInfo }
};

static void ExecuteType0Commands(Service::Interface* self) {
    u32 * cmd_buff = Kernel::GetCommandBuffer();
    u16 commands_offset = (u16)cmd_buff[1];
	printf("start command chain\r\n");
    while (commands_offset != 0xFFFF)
    {
        Command* command = (Command*)state.shared_memory->GetPointer(commands_offset).ValueOr(nullptr);
        commands_offset = command->next_command_offset;
        auto function = type0_commands.at(command->id);

		printf("handling %d ", command->id);
        if (function != nullptr)
        {
            function(command->params);
        }
		else
		{
			printf("unimplemented");
		}
		printf("\r\n");

		// seems like status code
		command->unk = 1;
    }
	printf("end command chain\r\n");


    cmd_buff[1] = RESULT_SUCCESS.raw;
}

static void ExecuteType1Commands(Service::Interface* self);
static void AcquireSoundChannels(Service::Interface* self)
{
	u32 * cmd_buff = Kernel::GetCommandBuffer();
	cmd_buff[2] = 0x1f;
	cmd_buff[1] = RESULT_SUCCESS.raw;
}
static void ReleaseSoundChannels(Service::Interface* self);
static void AcquireCaptureDevice(Service::Interface* self);
static void ReleaseCaptureDevice(Service::Interface* self);
static void FlushDCache(Service::Interface* self);
static void StoreDCache(Service::Interface* self);
static void InvalidateDCache(Service::Interface* self);

const Interface::FunctionInfo FunctionTable[] = {
    { 0x00010140, Initialize, "Initialize" },
    {0x00020000, nullptr,               "Shutdown"},
    { 0x00030040, ExecuteType0Commands, "ExecuteType0Commands" },
    {0x00040080, nullptr,               "ExecuteType1Commands"},
	{ 0x00050000, AcquireSoundChannels, "AcquireSoundChannels" },
    {0x00060000, nullptr,               "ReleaseSoundChannels"},
    {0x00070000, nullptr,               "AcquireCaptureDevice"},
    {0x00080040, nullptr,               "ReleaseCaptureDevice"},
    {0x00090082, nullptr,               "FlushDCache"},
    {0x000A0082, nullptr,               "StoreDCache"},
    {0x000B0082, nullptr,               "InvalidateDCache"},
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// Interface class

Interface::Interface() {
    Register(FunctionTable);
}

} // namespace
