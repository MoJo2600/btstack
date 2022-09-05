/*
 * Copyright (C) 2022 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

#define BTSTACK_FILE__ "le_audio_broadcast_sink.c"

/*
 * LE Audio Broadcast Sink
 */


#include "btstack_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <fcntl.h>        // open
#include <errno.h>

#include "ad_parser.h"
#include "bluetooth_data_types.h"
#include "bluetooth_gatt.h"
#include "btstack_debug.h"
#include "btstack_audio.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "btstack_ring_buffer.h"
#include "btstack_stdin.h"
#include "btstack_util.h"
#include "gap.h"
#include "hci.h"
#include "hci_cmd.h"
#include "btstack_lc3.h"
#include "btstack_lc3_google.h"
#include "btstack_lc3plus_fraunhofer.h"

#ifdef HAVE_POSIX_FILE_IO
#include "wav_util.h"
#endif

// max config
#define MAX_NUM_BIS 2
#define MAX_SAMPLES_PER_FRAME 480

// playback
#define MAX_NUM_LC3_FRAMES   5
#define MAX_BYTES_PER_SAMPLE 4
#define PLAYBACK_BUFFER_SIZE (MAX_NUM_LC3_FRAMES * MAX_SAMPLES_PER_FRAME * MAX_BYTES_PER_SAMPLE)

// analysis
#define PACKET_PREFIX_LEN 10

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

static void show_usage(void);

static const char * filename_wav = "le_audio_broadcast_sink.wav";

static enum {
    APP_W4_WORKING,
    APP_W4_BROADCAST_ADV,
    APP_W4_PA_AND_BIG_INFO,
    APP_W4_BIG_SYNC_ESTABLISHED,
    APP_STREAMING,
    APP_IDLE
} app_state = APP_W4_WORKING;

//
static btstack_packet_callback_registration_t hci_event_callback_registration;

static bool have_base;
static bool have_big_info;

uint32_t last_samples_report_ms;
uint16_t samples_received;
uint16_t samples_dropped;
uint16_t frames_per_second[MAX_NUM_BIS];

// remote info
static char remote_name[20];
static bd_addr_t remote;
static bd_addr_type_t remote_type;
static uint8_t remote_sid;
static bool count_mode;
static bool pts_mode;
static bool nrf5340_audio_demo;


// broadcast info
static const uint8_t    big_handle = 1;
static hci_con_handle_t sync_handle;
static hci_con_handle_t bis_con_handles[MAX_NUM_BIS];
static unsigned int     next_bis_index;

// analysis
static bool     last_packet_received[MAX_NUM_BIS];
static uint16_t last_packet_sequence[MAX_NUM_BIS];
static uint32_t last_packet_time_ms[MAX_NUM_BIS];
static uint8_t  last_packet_prefix[MAX_NUM_BIS * PACKET_PREFIX_LEN];

// BIG Sync
static le_audio_big_sync_t        big_sync_storage;
static le_audio_big_sync_params_t big_sync_params;

// lc3 writer
static int dump_file;
static uint32_t lc3_frames;

// lc3 codec config
static uint16_t sampling_frequency_hz;
static btstack_lc3_frame_duration_t frame_duration;
static uint16_t number_samples_per_frame;
static uint16_t octets_per_frame;
static uint8_t  num_bis;

// lc3 decoder
static bool request_lc3plus_decoder = false;
static bool use_lc3plus_decoder = false;
static const btstack_lc3_decoder_t * lc3_decoder;
static int16_t pcm[MAX_NUM_BIS * MAX_SAMPLES_PER_FRAME];

static btstack_lc3_decoder_google_t google_decoder_contexts[MAX_NUM_BIS];
#ifdef HAVE_LC3PLUS
static btstack_lc3plus_fraunhofer_decoder_t fraunhofer_decoder_contexts[MAX_NUM_BIS];
#endif
static void * decoder_contexts[MAX_NR_BIS];

// playback
static uint8_t playback_buffer_storage[PLAYBACK_BUFFER_SIZE];
static btstack_ring_buffer_t playback_buffer;

static btstack_timer_source_t next_packet_timer[MAX_NUM_BIS];
static uint16_t               cached_iso_sdu_len;
static bool                   have_pcm[MAX_NUM_BIS];

static void le_audio_broadcast_sink_playback(int16_t * buffer, uint16_t num_samples){
    // called from lower-layer but guaranteed to be on main thread
    uint32_t bytes_needed = num_samples * num_bis * 2;

    static bool underrun = true;

    log_info("Playback: need %u, have %u", num_samples, btstack_ring_buffer_bytes_available(&playback_buffer) / ( num_bis * 2));

    if (bytes_needed > btstack_ring_buffer_bytes_available(&playback_buffer)){
        memset(buffer, 0, bytes_needed);
        if (underrun == false){
            log_info("Playback underrun");
            underrun = true;
        }
        return;
    }

    if (underrun){
        underrun = false;
        log_info("Playback started");
    }
    uint32_t bytes_read;
    btstack_ring_buffer_read(&playback_buffer, (uint8_t *) buffer, bytes_needed, &bytes_read);
    btstack_assert(bytes_read == bytes_needed);
}

static void setup_lc3_decoder(void){
    uint8_t channel;
    for (channel = 0 ; channel < num_bis ; channel++){
        // pick decoder
        void * decoder_context = NULL;
#ifdef HAVE_LC3PLUS
        if (use_lc3plus_decoder){
            decoder_context = &fraunhofer_decoder_contexts[channel];
            lc3_decoder = btstack_lc3plus_fraunhofer_decoder_init_instance(decoder_context);
        }
        else
#endif
        {
            decoder_context = &google_decoder_contexts[channel];
            lc3_decoder = btstack_lc3_decoder_google_init_instance(decoder_context);
        }
        decoder_contexts[channel] = decoder_context;
        lc3_decoder->configure(decoder_context, sampling_frequency_hz, frame_duration);
    }
    number_samples_per_frame = lc3_decoder->get_number_samples_per_frame(decoder_contexts[0]);
    btstack_assert(number_samples_per_frame <= MAX_SAMPLES_PER_FRAME);
}

static void close_files(void){
#ifdef HAVE_POSIX_FILE_IO
    printf("Close files\n");
    close(dump_file);
    wav_writer_close();
#endif
}

static void handle_periodic_advertisement(const uint8_t * packet, uint16_t size){
    // nRF534_audio quirk - no BASE in periodic advertisement
    if (nrf5340_audio_demo){
        // hard coded config LC3
        // default: mono bitrate 96000, 10 ms with USB audio source, 120 octets per frame
        count_mode = 0;
        pts_mode   = 0;
        num_bis    = 1;
        sampling_frequency_hz = 48000;
        frame_duration = BTSTACK_LC3_FRAME_DURATION_10000US;
        octets_per_frame = 120;
        have_base = true;
        return;
    }

    // periodic advertisement contains the BASE
    // TODO: BASE might be split across multiple advertisements
    const uint8_t * adv_data = hci_subevent_le_periodic_advertising_report_get_data(packet);
    uint16_t adv_size = hci_subevent_le_periodic_advertising_report_get_data_length(packet);
    uint8_t adv_status = hci_subevent_le_periodic_advertising_report_get_data_status(packet);

    if (adv_status != 0) {
        printf("Periodic Advertisement (status %u): ", adv_status);
        printf_hexdump(adv_data, adv_size);
        return;
    }

    ad_context_t context;
    for (ad_iterator_init(&context, adv_size, adv_data) ; ad_iterator_has_more(&context) ; ad_iterator_next(&context)) {
        uint8_t data_type = ad_iterator_get_data_type(&context);
        // TODO: avoid out-of-bounds read
        // uint8_t data_size = ad_iterator_get_data_len(&context);
        const uint8_t * data = ad_iterator_get_data(&context);
        uint16_t uuid;
        switch (data_type){
            case BLUETOOTH_DATA_TYPE_SERVICE_DATA_16_BIT_UUID:
                uuid = little_endian_read_16(data, 0);
                if (uuid == ORG_BLUETOOTH_SERVICE_BASIC_AUDIO_ANNOUNCEMENT_SERVICE){
                    have_base = true;
                    // Level 1: Group Level
                    const uint8_t * base_data = &data[2];
                    // TODO: avoid out-of-bounds read
                    // uint16_t base_len = data_size - 2;
                    printf("BASE:\n");
                    uint32_t presentation_delay = little_endian_read_24(base_data, 0);
                    printf("- presentation delay: %"PRIu32" us\n", presentation_delay);
                    uint8_t num_subgroups = base_data[3];
                    printf("- num subgroups: %u\n", num_subgroups);
                    uint8_t i;
                    uint16_t offset = 4;
                    for (i=0;i<num_subgroups;i++){
                        // Level 2: Subgroup Level
                        num_bis = base_data[offset++];
                        printf("  - num bis[%u]: %u\n", i, num_bis);
                        // codec_id: coding format = 0x06, vendor and coded id = 0
                        offset += 5;
                        uint8_t codec_specific_configuration_length = base_data[offset++];
                        const uint8_t * codec_specific_configuration = &base_data[offset];
                        printf("  - codec specific config[%u]: ", i);
                        printf_hexdump(codec_specific_configuration, codec_specific_configuration_length);
                        // parse config to get sampling frequency and frame duration
                        uint8_t codec_offset = 0;
                        while ((codec_offset + 1) < codec_specific_configuration_length){
                            uint8_t ltv_len = codec_specific_configuration[codec_offset++];
                            uint8_t ltv_type = codec_specific_configuration[codec_offset];
                            const uint32_t sampling_frequency_map[] = { 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000, 176400, 192000, 384000 };
                            uint8_t sampling_frequency_index;
                            uint8_t frame_duration_index;
                            switch (ltv_type){
                                case 0x01: // sampling frequency
                                    sampling_frequency_index = codec_specific_configuration[codec_offset+1];
                                    // TODO: check range
                                    sampling_frequency_hz = sampling_frequency_map[sampling_frequency_index - 1];
                                    printf("    - sampling frequency[%u]: %u\n", i, sampling_frequency_hz);
                                    break;
                                case 0x02: // 0 = 7.5, 1 = 10 ms
                                    frame_duration_index =  codec_specific_configuration[codec_offset+1];
                                    frame_duration = (frame_duration_index == 0) ? BTSTACK_LC3_FRAME_DURATION_7500US : BTSTACK_LC3_FRAME_DURATION_10000US;
                                    printf("    - frame duration[%u]: %s ms\n", i, (frame_duration == BTSTACK_LC3_FRAME_DURATION_7500US) ? "7.5" : "10");
                                    break;
                                case 0x04:  // octets per coding frame
                                    octets_per_frame = little_endian_read_16(codec_specific_configuration, codec_offset+1);
                                    printf("    - octets per codec frame[%u]: %u\n", i, octets_per_frame);
                                    break;
                                default:
                                    break;
                            }
                            codec_offset += ltv_len;
                        }
                        //
                        offset += codec_specific_configuration_length;
                        uint8_t metadata_length = base_data[offset++];
                        const uint8_t * meta_data = &base_data[offset];
                        offset += metadata_length;
                        printf("  - meta data[%u]: ", i);
                        printf_hexdump(meta_data, metadata_length);
                        uint8_t k;
                        for (k=0;k<num_bis;k++){
                            // Level 3: BIS Level
                            uint8_t bis_index = base_data[offset++];
                            printf("    - bis index[%u][%u]: %u\n", i, k, bis_index);
                            uint8_t codec_specific_configuration_length2 = base_data[offset++];
                            const uint8_t * codec_specific_configuration2 = &base_data[offset];
                            printf("    - codec specific config[%u][%u]: ", i, k);
                            printf_hexdump(codec_specific_configuration2, codec_specific_configuration_length2);
                            offset += codec_specific_configuration_length2;
                        }
                    }
                }
                break;
            default:
                break;
        }
    }
}

static void handle_big_info(const uint8_t * packet, uint16_t size){
    printf("BIG Info advertising report\n");
    sync_handle = hci_subevent_le_biginfo_advertising_report_get_sync_handle(packet);
    have_big_info = true;
}

static void enter_create_big_sync(void){
    // stop scanning
    gap_stop_scan();

    // switch to lc3plus if requested and possible
    use_lc3plus_decoder = request_lc3plus_decoder && (frame_duration == BTSTACK_LC3_FRAME_DURATION_10000US);

    // init decoder
    setup_lc3_decoder();

    printf("Configure: %u channels, sampling rate %u, samples per frame %u, lc3plus %u\n", num_bis, sampling_frequency_hz, number_samples_per_frame, use_lc3plus_decoder);

#ifdef HAVE_POSIX_FILE_IO
    // create wav file
    printf("WAV file: %s\n", filename_wav);
    wav_writer_open(filename_wav, num_bis, sampling_frequency_hz);
#endif

    // init playback buffer
    btstack_ring_buffer_init(&playback_buffer, playback_buffer_storage, PLAYBACK_BUFFER_SIZE);

    // start playback
    // PTS 8.2 sends stereo at half speed for stereo, for now playback at half speed
    const btstack_audio_sink_t * sink = btstack_audio_sink_get_instance();
    if (sink != NULL){
        uint16_t playback_speed;
        if ((num_bis > 1) && pts_mode){
            playback_speed = sampling_frequency_hz / num_bis;
            printf("PTS workaround: playback at %u hz\n", playback_speed);
        } else {
            playback_speed = sampling_frequency_hz;
        };
        sink->init(num_bis, sampling_frequency_hz, le_audio_broadcast_sink_playback);
        sink->start_stream();
    }

    big_sync_params.big_handle = big_handle;
    big_sync_params.sync_handle = sync_handle;
    big_sync_params.encryption = 0;
    memset(big_sync_params.broadcast_code, 0, 16);
    big_sync_params.mse = 0;
    big_sync_params.big_sync_timeout_10ms = 100;
    big_sync_params.num_bis = num_bis;
    uint8_t i;
    printf("BIG Create Sync for BIS: ");
    for (i=0;i<num_bis;i++){
        big_sync_params.bis_indices[i] = i + 1;
        printf("%u ", big_sync_params.bis_indices[i]);
    }
    printf("\n");
    app_state = APP_W4_BIG_SYNC_ESTABLISHED;
    gap_big_sync_create(&big_sync_storage, &big_sync_params);
}

static void start_scanning() {
    app_state = APP_W4_BROADCAST_ADV;
    gap_set_scan_params(1, 0x30, 0x30, 0);
    gap_start_scan();
    printf("Start scan..\n");
}

static void packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    if (packet_type != HCI_EVENT_PACKET) return;
    switch (packet[0]) {
        case BTSTACK_EVENT_STATE:
            switch(btstack_event_state_get_state(packet)) {
                case HCI_STATE_WORKING:
#ifdef ENABLE_DEMO_MODE
                    if (app_state != APP_W4_WORKING) break;
                    start_scanning();
#else
                    show_usage();
#endif
                    break;
                case HCI_STATE_OFF:
                    printf("Goodbye\n");
                    exit(0);
                    break;
                default:
                    break;
            }
            break;
        case GAP_EVENT_EXTENDED_ADVERTISING_REPORT:
        {
            if (app_state != APP_W4_BROADCAST_ADV) break;

            gap_event_extended_advertising_report_get_address(packet, remote);
            uint8_t adv_size = gap_event_extended_advertising_report_get_data_length(packet);
            const uint8_t * adv_data = gap_event_extended_advertising_report_get_data(packet);

            ad_context_t context;
            bool found = false;
            remote_name[0] = '\0';
            uint16_t uuid;
            for (ad_iterator_init(&context, adv_size, adv_data) ; ad_iterator_has_more(&context) ; ad_iterator_next(&context)) {
                uint8_t data_type = ad_iterator_get_data_type(&context);
                uint8_t size = ad_iterator_get_data_len(&context);
                const uint8_t *data = ad_iterator_get_data(&context);
                switch (data_type){
                    case BLUETOOTH_DATA_TYPE_SERVICE_DATA_16_BIT_UUID:
                        uuid = little_endian_read_16(data, 0);
                        if (uuid == ORG_BLUETOOTH_SERVICE_BROADCAST_AUDIO_ANNOUNCEMENT_SERVICE){
                            found = true;
                        }
                        break;
                    case BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME:
                    case BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME:
                        size = btstack_min(sizeof(remote_name) - 1, size);
                        memcpy(remote_name, data, size);
                        remote_name[size] = 0;
                        // support for nRF5340 Audio DK
                        if (strncmp("NRF5340", remote_name, 7) == 0){
                            nrf5340_audio_demo = true;
                            found = true;
                        }
                        break;
                    default:
                        break;
                }
            }
            if (!found) break;
            remote_type = gap_event_extended_advertising_report_get_address_type(packet);
            remote_sid = gap_event_extended_advertising_report_get_advertising_sid(packet);
            pts_mode = strncmp("PTS-", remote_name, 4) == 0;
            count_mode = strncmp("COUNT", remote_name, 5) == 0;
            printf("Remote Broadcast sink found, addr %s, name: '%s' (pts-mode: %u, count: %u)\n", bd_addr_to_str(remote), remote_name, pts_mode, count_mode);
            // ignore other advertisements
            gap_whitelist_add(remote_type, remote);
            gap_set_scan_params(1, 0x30, 0x30, 1);
            // sync to PA
            gap_periodic_advertiser_list_clear();
            gap_periodic_advertiser_list_add(remote_type, remote, remote_sid);
            app_state = APP_W4_PA_AND_BIG_INFO;
            printf("Start Periodic Advertising Sync\n");
            gap_periodic_advertising_create_sync(0x01, remote_sid, remote_type, remote, 0, 1000, 0);
            break;
        }

        case HCI_EVENT_LE_META:
            switch(hci_event_le_meta_get_subevent_code(packet)) {
                case HCI_SUBEVENT_LE_PERIODIC_ADVERTISING_SYNC_ESTABLISHMENT:
                    printf("Periodic advertising sync established\n");
                    break;
                case HCI_SUBEVENT_LE_PERIODIC_ADVERTISING_REPORT:
                    if (have_base) break;
                    handle_periodic_advertisement(packet, size);
                    if (have_base & have_big_info){
                        enter_create_big_sync();
                    }
                    break;
                case HCI_SUBEVENT_LE_BIGINFO_ADVERTISING_REPORT:
                    if (have_big_info) break;
                    handle_big_info(packet, size);
                    if (have_base & have_big_info){
                        enter_create_big_sync();
                    }
                    break;
                case HCI_SUBEVENT_LE_BIG_SYNC_LOST:
                    printf("BIG Sync Lost\n");
                    {
                        const btstack_audio_sink_t * sink = btstack_audio_sink_get_instance();
                        if (sink != NULL) {
                            sink->stop_stream();
                            sink->close();
                        }
                    }
                    // start over
                    start_scanning();
                    break;
                default:
                    break;
            }
            break;
        case HCI_EVENT_META_GAP:
            switch (hci_event_gap_meta_get_subevent_code(packet)){
                case GAP_SUBEVENT_BIG_SYNC_CREATED: {
                    printf("BIG Sync created with BIS Connection handles: ");
                    uint8_t i;
                    for (i=0;i<num_bis;i++){
                        bis_con_handles[i] = gap_subevent_big_sync_created_get_bis_con_handles(packet, i);
                        printf("0x%04x ", bis_con_handles[i]);
                    }
                    app_state = APP_STREAMING;
                    last_samples_report_ms = btstack_run_loop_get_time_ms();
                    memset(last_packet_sequence, 0, sizeof(last_packet_sequence));
                    memset(last_packet_received, 0, sizeof(last_packet_received));
                    printf("Start receiving\n");
                    break;
                }
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

static void store_samples_in_ringbuffer(void){
    // check if we have all channels
    uint8_t bis_channel;
    for (bis_channel = 0; bis_channel < num_bis ; bis_channel++){
        if (have_pcm[bis_channel] == false) return;
    }
#ifdef HAVE_POSIX_FILE_IO
    // write wav samples
    wav_writer_write_int16(num_bis * number_samples_per_frame, pcm);
#endif
    // store samples in playback buffer
    uint32_t bytes_to_store = num_bis * number_samples_per_frame * 2;
    samples_received += number_samples_per_frame;
    if (btstack_ring_buffer_bytes_free(&playback_buffer) >= bytes_to_store) {
        btstack_ring_buffer_write(&playback_buffer, (uint8_t *) pcm, bytes_to_store);
    } else {
        printf("Samples dropped\n");
        samples_dropped += number_samples_per_frame;
    }
    // reset
    for (bis_channel = 0; bis_channel < num_bis ; bis_channel++){
        have_pcm[bis_channel] = false;
    }
}

static void plc_do(uint8_t bis_channel) {// inject packet
    uint8_t tmp_BEC_detect;
    uint8_t BFI = 1;
    (void) lc3_decoder->decode_signed_16(decoder_contexts[bis_channel], NULL, cached_iso_sdu_len, BFI,
                                         &pcm[bis_channel], num_bis,
                                         &tmp_BEC_detect);

    printf("PLC channel %u - packet sequence %u\n", bis_channel,  last_packet_sequence[bis_channel]);

    have_pcm[bis_channel] = true;
    store_samples_in_ringbuffer();
}

static void plc_timeout(btstack_timer_source_t * timer) {

    uint8_t bis_channel = (uint8_t) (uintptr_t) btstack_run_loop_get_timer_context(timer);

    // Restart timer. This will loose sync with ISO interval, but if we stop caring if we loose that many packets
    uint32_t frame_duration_ms = frame_duration == BTSTACK_LC3_FRAME_DURATION_7500US ? 8 : 10;
    btstack_run_loop_set_timer(&next_packet_timer[bis_channel], frame_duration_ms);
    btstack_run_loop_set_timer_handler(&next_packet_timer[bis_channel], plc_timeout);
    btstack_run_loop_add_timer(&next_packet_timer[bis_channel]);

    last_packet_sequence[bis_channel]++;
    last_packet_time_ms[bis_channel] += frame_duration;
    plc_do(bis_channel);
}

static void iso_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){

    uint16_t header = little_endian_read_16(packet, 0);
    hci_con_handle_t con_handle = header & 0x0fff;
    uint8_t pb_flag = (header >> 12) & 3;
    uint8_t ts_flag = (header >> 14) & 1;
    uint16_t iso_load_len = little_endian_read_16(packet, 2);

    uint16_t offset = 4;
    uint32_t time_stamp = 0;
    if (ts_flag){
        uint32_t time_stamp = little_endian_read_32(packet, offset);
        offset += 4;
    }

    uint32_t receive_time_ms = btstack_run_loop_get_time_ms();

    uint16_t packet_sequence_number = little_endian_read_16(packet, offset);
    offset += 2;

    uint16_t header_2 = little_endian_read_16(packet, offset);
    uint16_t iso_sdu_length = header_2 & 0x3fff;
    uint8_t packet_status_flag = (uint8_t) (header_2 >> 14);
    offset += 2;

    if (iso_sdu_length == 0) return;

    // infer channel from con handle - only works for up to 2 channels
    uint8_t bis_channel = (con_handle == bis_con_handles[0]) ? 0 : 1;

    if (count_mode){
        // check for missing packet
        uint16_t last_seq_no = last_packet_sequence[bis_channel];
        bool packet_missed = (last_seq_no != 0) && ((last_seq_no + 1) != packet_sequence_number);
        if (packet_missed){
            // print last packet
            printf("\n");
            printf("%04x %10"PRIu32" %u ", last_seq_no, last_packet_time_ms[bis_channel], bis_channel);
            printf_hexdump(&last_packet_prefix[num_bis*PACKET_PREFIX_LEN], PACKET_PREFIX_LEN);
            last_seq_no++;

            printf(ANSI_COLOR_RED);
            while (last_seq_no < packet_sequence_number){
                printf("%04x            %u MISSING\n", last_seq_no, bis_channel);
                last_seq_no++;
            }
            printf(ANSI_COLOR_RESET);

            // print current packet
            printf("%04x %10"PRIu32" %u ", packet_sequence_number, receive_time_ms, bis_channel);
            printf_hexdump(&packet[offset], PACKET_PREFIX_LEN);
        }

        // cache current packet
        memcpy(&last_packet_prefix[num_bis*PACKET_PREFIX_LEN], &packet[offset], PACKET_PREFIX_LEN);

    } else {

        if (last_packet_received[bis_channel]){
            int16_t packet_sequence_delta = btstack_time16_delta(packet_sequence_number, last_packet_sequence[bis_channel]);
            if (packet_sequence_delta < 1){
                // drop delayed packet that had already been generated by PLC
                printf("Dropping delayed packet. Current sequence number %u, last received or generated by PLC: %u\n",
                       packet_sequence_number, last_packet_sequence[bis_channel]);
                return;
            }
        } else {
            last_packet_received[bis_channel] = true;
        }

        // decode codec frame
        uint8_t tmp_BEC_detect;
        uint8_t BFI = 0;
        (void) lc3_decoder->decode_signed_16(decoder_contexts[bis_channel], &packet[offset], iso_sdu_length, BFI,
                                   &pcm[bis_channel], num_bis,
                                   &tmp_BEC_detect);
        have_pcm[bis_channel] = true;
        store_samples_in_ringbuffer();

        lc3_frames++;
        frames_per_second[bis_channel]++;

        // PLC
        cached_iso_sdu_len = iso_sdu_length;
        uint32_t frame_duration_ms = frame_duration == BTSTACK_LC3_FRAME_DURATION_7500US ? 8 : 10;
        uint32_t timeout_ms = frame_duration_ms * 3 / 2;
        btstack_run_loop_remove_timer(&next_packet_timer[bis_channel]);
        btstack_run_loop_set_timer(&next_packet_timer[bis_channel], timeout_ms);
        btstack_run_loop_set_timer_context(&next_packet_timer[bis_channel], (void *) (uintptr_t) bis_channel);
        btstack_run_loop_set_timer_handler(&next_packet_timer[bis_channel], plc_timeout);
        btstack_run_loop_add_timer(&next_packet_timer[bis_channel]);

        uint32_t time_ms = btstack_run_loop_get_time_ms();
        if (btstack_time_delta(time_ms, last_samples_report_ms) >= 1000){
            last_samples_report_ms = time_ms;
            printf("LC3 Frames: %4u - ", (int) (lc3_frames / num_bis));
            uint8_t i;
            for (i=0;i<num_bis;i++){
                printf("%u ", frames_per_second[i]);
                frames_per_second[i] = 0;
            }
            printf(" frames per second, dropped %u of %u\n", samples_dropped, samples_received);
            samples_received = 0;
            samples_dropped  =  0;
        }
    }

    last_packet_time_ms[bis_channel]  = receive_time_ms;
    last_packet_sequence[bis_channel] = packet_sequence_number;
}

static void show_usage(void){
    printf("\n--- LE Audio Broadcast Sink Test Console ---\n");
    printf("s - start scanning\n");
#ifdef HAVE_LC3PLUS
    printf("q - use LC3plus decoder if 10 ms ISO interval is used\n");
#endif
    printf("x - close files and exit\n");
    printf("---\n");
}

static void stdin_process(char c){
    switch (c){
        case 's':
            if (app_state != APP_W4_WORKING) break;
            start_scanning();
            break;
#ifdef HAVE_LC3PLUS
        case 'q':
            printf("Use LC3plus decoder for 10 ms ISO interval...\n");
            request_lc3plus_decoder = true;
            break;
#endif
        case 'x':
            close_files();
            printf("Shutdown...\n");
            hci_power_control(HCI_POWER_OFF);
            break;
        case '\n':
        case '\r':
            break;
        default:
            show_usage();
            break;

    }
}

int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){
    (void) argv;
    (void) argc;
    
    // register for HCI events
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // register for ISO Packet
    hci_register_iso_packet_handler(&iso_packet_handler);

    // turn on!
    hci_power_control(HCI_POWER_ON);

    btstack_stdin_setup(stdin_process);
    return 0;
}
