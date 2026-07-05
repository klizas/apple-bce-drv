#ifndef AAUDIO_H
#define AAUDIO_H

#include <linux/types.h>
#include <linux/hrtimer.h>
#include <sound/pcm.h>
#include "../apple_bce.h"
#include "protocol_bce.h"
#include "description.h"

#define AAUDIO_SIG 0x19870423

#define AAUDIO_DEVICE_MAX_UID_LEN 128
#define AAUDIO_DEVICE_MAX_INPUT_STREAMS 1
#define AAUDIO_DEVICE_MAX_OUTPUT_STREAMS 1
#define AAUDIO_DEVICE_MAX_BUFFER_COUNT 1

#define AAUDIO_BUFFER_ID_NONE 0xffu

struct snd_card;
struct snd_pcm;
struct snd_pcm_hardware;
struct snd_jack;

struct __attribute__((packed)) __attribute__((aligned(4))) aaudio_buffer_struct_buffer {
    size_t address;
    size_t size;
    size_t pad[4];
};
struct aaudio_buffer_struct_stream {
    u8 num_buffers;
    struct aaudio_buffer_struct_buffer buffers[100];
    char filler[32];
};
struct aaudio_buffer_struct_device {
    char name[128];
    u8 num_input_streams;
    u8 num_output_streams;
    struct aaudio_buffer_struct_stream input_streams[5];
    struct aaudio_buffer_struct_stream output_streams[5];
    char filler[128];
};
struct aaudio_buffer_struct {
    u32 version;
    u32 signature;
    u32 flags;
    u8 num_devices;
    struct aaudio_buffer_struct_device devices[20];
};

struct aaudio_device;
struct aaudio_dma_buf {
    dma_addr_t dma_addr;
    void *ptr;
    size_t size;
};
struct aaudio_stream {
    aaudio_object_id_t id;
    size_t buffer_cnt;
    struct aaudio_dma_buf *buffers;

    struct aaudio_apple_description desc;
    struct snd_pcm_hardware *alsa_hw_desc;
    u32 latency;
    bool host_allocated;

    bool waiting_for_first_ts;
    s64 buffer_time_ns; /* buffer duration, precomputed in .prepare for the pointer callback */

    ktime_t remote_timestamp;
    snd_pcm_sframes_t frame_min;
    int started;

    /* The T2 only signals once per pass over the whole ring, so period
     * wakeups come from this self-rearming hrtimer instead; the pointer
     * callback interpolates the position between signals. The timer only
     * queues period_work — the PCM is nonatomic, so
     * snd_pcm_period_elapsed may not be called from timer context. */
    struct hrtimer period_timer;
    struct work_struct period_work;
    struct snd_pcm_substream *pcm_substream;
    u64 period_time_ns;
};
struct aaudio_subdevice {
    struct aaudio_device *a;
    struct list_head list;
    aaudio_device_id_t dev_id;
    /* Latest timestamp pair from the T2, consumed by ts_work. The PCM
     * stream locks may not be taken in the BCE completion thread: a
     * trigger holds them while waiting for its start/stop_io reply,
     * which is delivered by that same thread. */
    struct work_struct ts_work;
    struct spinlock ts_lock;
    ktime_t ts_time_os;
    u64 ts_dev_timestamp;
    u32 in_latency, out_latency;
    u8 buf_id;
    int alsa_id;
    char uid[AAUDIO_DEVICE_MAX_UID_LEN + 1];
    size_t in_stream_cnt;
    struct aaudio_stream in_streams[AAUDIO_DEVICE_MAX_INPUT_STREAMS];
    size_t out_stream_cnt;
    struct aaudio_stream out_streams[AAUDIO_DEVICE_MAX_OUTPUT_STREAMS];
    bool is_pcm;
    struct snd_pcm *pcm;
    struct snd_jack *jack;
};
struct aaudio_alsa_pcm_id_mapping {
    const char *name;
    int alsa_id;
};

struct aaudio_device {
    struct pci_dev *pci;
    dev_t devt;
    struct device *dev;
    void __iomem *reg_mem_bs;
    dma_addr_t reg_mem_bs_dma;
    void __iomem *reg_mem_cfg;

    u32 __iomem *reg_mem_gpr;

    struct aaudio_buffer_struct *bs;

    struct apple_bce_device *bce;
    struct aaudio_bce bcem;

    struct snd_card *card;

    struct list_head subdevice_list;
    int next_alsa_id;

    struct completion remote_alive;
};

void aaudio_handle_notification(struct aaudio_device *a, struct aaudio_msg *msg);
void aaudio_handle_prop_change_work(struct work_struct *ws);
void aaudio_handle_cmd_timestamp(struct aaudio_device *a, struct aaudio_msg *msg);
void aaudio_handle_command(struct aaudio_device *a, struct aaudio_msg *msg);

int aaudio_module_init(void);
void aaudio_module_exit(void);

extern struct aaudio_alsa_pcm_id_mapping aaudio_alsa_id_mappings[];

#endif //AAUDIO_H
