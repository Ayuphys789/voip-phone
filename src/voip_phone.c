#include <arpa/inet.h>
#include <gtk/gtk.h>
#include <math.h>
#include <portaudio.h>
#include <pthread.h>
#include <speex/speex_echo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SAMPLE_RATE (44100)
#define NUM_CHANNELS (1)
#define FRAMES_PER_BUFFER (512)
#define PA_SAMPLE_TYPE paInt16
typedef short SAMPLE;
#define RING_BUFFER_MILLISECONDS (300)
#define RING_BUFFER_SIZE ((SAMPLE_RATE * RING_BUFFER_MILLISECONDS) / 1000)
#define TAIL_LENGTH_MS (120)

typedef struct
{
    uint32_t sequence_number;
    SAMPLE audio_data[FRAMES_PER_BUFFER];
} AudioPacket;

typedef struct
{
    AudioPacket buffer[20];
    gboolean slot_filled[20];
    volatile uint32_t next_seq_to_play;
    volatile gboolean is_primed;
    int min_playout_delay;
    pthread_mutex_t mutex;
} JitterBuffer;

typedef struct
{
    SAMPLE *buffer;
    volatile int write_pos;
    volatile int read_pos;
    int size;
    pthread_mutex_t mutex;
} RingBuffer;

void rb_init(RingBuffer *rb, int size)
{
    rb->buffer = (SAMPLE *)malloc(size * sizeof(SAMPLE));
    memset(rb->buffer, 0, size * sizeof(SAMPLE));
    rb->write_pos = 0;
    rb->read_pos = 0;
    rb->size = size;
    pthread_mutex_init(&rb->mutex, NULL);
}

void rb_destroy(RingBuffer *rb)
{
    free(rb->buffer);
    pthread_mutex_destroy(&rb->mutex);
}

int rb_write(RingBuffer *rb, const SAMPLE *data, int count)
{
    pthread_mutex_lock(&rb->mutex);
    for (int i = 0; i < count; i++)
    {
        rb->buffer[rb->write_pos] = data[i];
        rb->write_pos = (rb->write_pos + 1) % rb->size;
    }
    pthread_mutex_unlock(&rb->mutex);
    return count;
}

int rb_read(RingBuffer *rb, SAMPLE *data, int count)
{
    pthread_mutex_lock(&rb->mutex);
    for (int i = 0; i < count; i++)
    {
        data[i] = rb->buffer[rb->read_pos];
        rb->read_pos = (rb->read_pos + 1) % rb->size;
    }
    pthread_mutex_unlock(&rb->mutex);
    return count;
}

void jitter_buffer_init(JitterBuffer *jb)
{
    memset(jb->buffer, 0, sizeof(jb->buffer));
    memset(jb->slot_filled, 0, sizeof(jb->slot_filled));
    jb->next_seq_to_play = 0;
    jb->is_primed = FALSE;
    jb->min_playout_delay = 4;
    pthread_mutex_init(&jb->mutex, NULL);
}

void jitter_buffer_destroy(JitterBuffer *jb)
{
    pthread_mutex_destroy(&jb->mutex);
}

void jitter_buffer_put(JitterBuffer *jb, const AudioPacket *packet)
{
    pthread_mutex_lock(&jb->mutex);
    if (jb->is_primed && packet->sequence_number < jb->next_seq_to_play)
    {
        pthread_mutex_unlock(&jb->mutex);
        return;
    }
    uint32_t index = packet->sequence_number % 20;
    jb->buffer[index] = *packet;
    jb->slot_filled[index] = TRUE;
    pthread_mutex_unlock(&jb->mutex);
}

void jitter_buffer_get(JitterBuffer *jb, SAMPLE *out_buffer, int frames)
{
    pthread_mutex_lock(&jb->mutex);
    if (!jb->is_primed)
    {
        int filled_count = 0;
        uint32_t lowest_seq = (uint32_t)-1;
        for (int i = 0; i < 20; i++)
        {
            if (jb->slot_filled[i])
            {
                filled_count++;
                if (jb->buffer[i].sequence_number < lowest_seq)
                {
                    lowest_seq = jb->buffer[i].sequence_number;
                }
            }
        }
        if (filled_count >= jb->min_playout_delay)
        {
            jb->next_seq_to_play = lowest_seq;
            jb->is_primed = TRUE;
        }
        else
        {
            memset(out_buffer, 0, frames * sizeof(SAMPLE));
            pthread_mutex_unlock(&jb->mutex);
            return;
        }
    }
    uint32_t index = jb->next_seq_to_play % 20;
    if (jb->slot_filled[index])
    {
        memcpy(out_buffer, jb->buffer[index].audio_data,
               frames * sizeof(SAMPLE));
        jb->slot_filled[index] = FALSE;
    }
    else
    {
        memset(out_buffer, 0, frames * sizeof(SAMPLE));
    }
    jb->next_seq_to_play++;
    pthread_mutex_unlock(&jb->mutex);
}

typedef struct
{
    gboolean is_running;
    gchar *peer_ip;
    int local_port;
    int peer_port;
    int send_sock;
    int recv_sock;
    gboolean is_muted;
    GtkLabel *status_label;
    GtkEntry *peer_ip_entry;
    GtkEntry *peer_port_entry;
    GtkEntry *local_port_entry;
    GtkWidget *call_button;
    GtkWidget *hangup_button;
    GtkWidget *mute_button;
    PaStream *stream;
    pthread_mutex_t mutex;
    float gain_factor;
    float noise_gate_threshold;
    GtkScale *gain_slider;
    GtkScale *threshold_slider;
    GtkLabel *timer_label;
    guint timer_id;
    int elapsed_seconds;
    gboolean timer_started;
    uint32_t send_sequence_number;
    RingBuffer send_rb;
    JitterBuffer jitter_buffer;
    SpeexEchoState *echo_state;
    GtkProgressBar *mic_level_bar;
    volatile float mic_rms_level;
    guint ui_update_timer_id;
} AppState;

static int pa_callback(const void *inputBuffer, void *outputBuffer,
                       unsigned long framesPerBuffer,
                       const PaStreamCallbackTimeInfo *timeInfo,
                       PaStreamCallbackFlags statusFlags, void *userData)
{
    AppState *state = (AppState *)userData;
    const SAMPLE *mic_in = (const SAMPLE *)inputBuffer;
    SAMPLE *speaker_out = (SAMPLE *)outputBuffer;
    SAMPLE aec_out[framesPerBuffer];

    jitter_buffer_get(&state->jitter_buffer, speaker_out, framesPerBuffer);

    if (mic_in != NULL)
    {
        speex_echo_playback(state->echo_state, speaker_out);
        speex_echo_capture(state->echo_state, mic_in, aec_out);

        float sum_of_squares = 0.0f;
        for (unsigned long i = 0; i < framesPerBuffer; i++)
        {
            sum_of_squares += (float)aec_out[i] * (float)aec_out[i];
        }
        float rms = sqrtf(sum_of_squares / framesPerBuffer);

        state->mic_rms_level = rms;

        if (rms > state->noise_gate_threshold)
        {
            SAMPLE temp_buffer[framesPerBuffer];
            for (unsigned long i = 0; i < framesPerBuffer; i++)
            {
                float boosted_sample = (float)aec_out[i] * state->gain_factor;
                if (boosted_sample > 32767.0f)
                    boosted_sample = 32767.0f;
                if (boosted_sample < -32768.0f)
                    boosted_sample = -32768.0f;
                temp_buffer[i] = (SAMPLE)boosted_sample;
            }
            rb_write(&state->send_rb, temp_buffer, framesPerBuffer);
        }
        else
        {
            SAMPLE silence_buffer[framesPerBuffer];
            memset(silence_buffer, 0, sizeof(silence_buffer));
            rb_write(&state->send_rb, silence_buffer, framesPerBuffer);
        }
    }
    return paContinue;
}

static gboolean update_ui_callback(gpointer user_data)
{
    AppState *state = (AppState *)user_data;
    if (!state->is_running)
    {
        state->ui_update_timer_id = 0;
        return G_SOURCE_REMOVE;
    }

    float fraction = state->mic_rms_level / 3000.0f;
    if (fraction > 1.0f)
        fraction = 1.0f;
    gtk_progress_bar_set_fraction(state->mic_level_bar, fraction);

    return G_SOURCE_CONTINUE;
}

void *sender_thread_func(void *data)
{
    AppState *state = (AppState *)data;
    AudioPacket packet;
    struct sockaddr_in peer_addr;
    memset(&peer_addr, 0, sizeof(peer_addr));
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(state->peer_port);
    inet_pton(AF_INET, state->peer_ip, &peer_addr.sin_addr);
    printf("[SENDER] Sender thread started.\n");
    while (state->is_running)
    {
        rb_read(&state->send_rb, packet.audio_data, FRAMES_PER_BUFFER);
        packet.sequence_number = state->send_sequence_number++;
        sendto(state->send_sock, &packet, sizeof(AudioPacket), 0,
               (struct sockaddr *)&peer_addr, sizeof(peer_addr));
        usleep(10000);
    }
    printf("[SENDER] Sender thread finished.\n");
    return NULL;
}

static gboolean update_timer_callback(gpointer user_data);
static gboolean start_timer_from_thread(gpointer user_data);

void *receiver_thread_func(void *data)
{
    AppState *state = (AppState *)data;
    AudioPacket packet;
    ssize_t bytes_received;
    printf("[RECEIVER] Receiver thread started.\n");
    while (state->is_running)
    {
        bytes_received = recvfrom(state->recv_sock, &packet,
                                  sizeof(AudioPacket), 0, NULL, NULL);
        if (bytes_received == sizeof(AudioPacket))
        {
            pthread_mutex_lock(&state->jitter_buffer.mutex);
            if (state->jitter_buffer.is_primed && !state->timer_started)
            {
                g_idle_add(start_timer_from_thread, state);
                state->timer_started = TRUE;
            }
            pthread_mutex_unlock(&state->jitter_buffer.mutex);
            jitter_buffer_put(&state->jitter_buffer, &packet);
        }
        else if (bytes_received <= 0 && state->is_running)
        {
            break;
        }
    }
    printf("[RECEIVER] Receiver thread finished.\n");
    return NULL;
}

static gboolean update_timer_callback(gpointer user_data)
{
    AppState *state = (AppState *)user_data;
    if (!state->is_running)
    {
        state->timer_id = 0;
        return G_SOURCE_REMOVE;
    }
    state->elapsed_seconds++;
    int minutes = state->elapsed_seconds / 60;
    int seconds = state->elapsed_seconds % 60;
    char time_str[16];
    sprintf(time_str, "Time: %02d:%02d", minutes, seconds);
    gtk_label_set_text(state->timer_label, time_str);
    return G_SOURCE_CONTINUE;
}

static gboolean start_timer_from_thread(gpointer user_data)
{
    AppState *state = (AppState *)user_data;
    if (state->timer_id == 0 && state->is_running)
    {
        state->elapsed_seconds = 0;
        gtk_label_set_text(state->timer_label, "Time: 00:00");
        state->timer_id =
            g_timeout_add_seconds(1, update_timer_callback, state);
    }
    return G_SOURCE_REMOVE;
}

void on_gain_slider_changed(GtkRange *range, gpointer user_data)
{
    AppState *state = (AppState *)user_data;
    pthread_mutex_lock(&state->mutex);
    state->gain_factor = (float)gtk_range_get_value(range);
    pthread_mutex_unlock(&state->mutex);
}

void on_threshold_slider_changed(GtkRange *range, gpointer user_data)
{
    AppState *state = (AppState *)user_data;
    pthread_mutex_lock(&state->mutex);
    state->noise_gate_threshold = (float)gtk_range_get_value(range);
    pthread_mutex_unlock(&state->mutex);
}

void on_mute_button_toggled(GtkToggleButton *button, gpointer user_data)
{
    AppState *state = (AppState *)user_data;
    gboolean is_active = gtk_toggle_button_get_active(button);
    pthread_mutex_lock(&state->mutex);
    state->is_muted = is_active;
    pthread_mutex_unlock(&state->mutex);
    printf("[INFO] Mute %s\n", is_active ? "ON" : "OFF");
}

void on_call_button_clicked(GtkButton *button, gpointer user_data)
{
    AppState *state = (AppState *)user_data;
    PaError err;
    const char *peer_ip_str =
        gtk_editable_get_text(GTK_EDITABLE(state->peer_ip_entry));
    const char *peer_port_str =
        gtk_editable_get_text(GTK_EDITABLE(state->peer_port_entry));
    const char *local_port_str =
        gtk_editable_get_text(GTK_EDITABLE(state->local_port_entry));
    pthread_mutex_lock(&state->mutex);
    if (state->peer_ip)
        g_free(state->peer_ip);
    state->peer_ip = g_strdup(peer_ip_str);
    state->peer_port = atoi(peer_port_str);
    state->local_port = atoi(local_port_str);
    state->is_running = TRUE;
    state->timer_started = FALSE;
    state->send_sequence_number = 0;
    pthread_mutex_unlock(&state->mutex);
    state->send_sock = socket(AF_INET, SOCK_DGRAM, 0);
    state->recv_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(state->local_port);
    if (bind(state->recv_sock, (struct sockaddr *)&local_addr,
             sizeof(local_addr)) == -1)
    {
        perror("bind() failed");
        return;
    }
    rb_init(&state->send_rb, RING_BUFFER_SIZE);
    jitter_buffer_init(&state->jitter_buffer);
    int tail_length_samples = (SAMPLE_RATE * TAIL_LENGTH_MS) / 1000;
    state->echo_state =
        speex_echo_state_init(FRAMES_PER_BUFFER, tail_length_samples);
    speex_echo_ctl(state->echo_state, SPEEX_ECHO_SET_SAMPLING_RATE,
                   (void *)&(int){SAMPLE_RATE});
    err = Pa_Initialize();
    if (err != paNoError)
        goto error;
    err = Pa_OpenDefaultStream(&state->stream, NUM_CHANNELS, NUM_CHANNELS,
                               PA_SAMPLE_TYPE, SAMPLE_RATE, FRAMES_PER_BUFFER,
                               pa_callback, state);
    if (err != paNoError)
        goto error;
    err = Pa_StartStream(state->stream);
    if (err != paNoError)
        goto error;

    if (state->ui_update_timer_id == 0)
    {
        state->ui_update_timer_id =
            g_timeout_add(50, update_ui_callback, state);
    }

    pthread_t sender_tid, receiver_tid;
    pthread_create(&sender_tid, NULL, sender_thread_func, state);
    pthread_create(&receiver_tid, NULL, receiver_thread_func, state);
    pthread_detach(sender_tid);
    pthread_detach(receiver_tid);

    gtk_label_set_text(state->timer_label, "Time: --:--");
    gtk_widget_set_visible(GTK_WIDGET(state->timer_label), TRUE);
    gtk_label_set_text(state->status_label, "Status: Calling...");
    gtk_widget_set_sensitive(state->call_button, FALSE);
    gtk_widget_set_sensitive(state->hangup_button, TRUE);
    gtk_widget_set_sensitive(state->mute_button, TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(state->gain_slider), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(state->threshold_slider), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(state->mic_level_bar),
                           TRUE);
    printf("[INFO] Call initiated.\n");
    return;
error:
    fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
    if (state->stream)
    {
        Pa_CloseStream(state->stream);
        state->stream = NULL;
    }
    Pa_Terminate();
    if (state->echo_state)
    {
        speex_echo_state_destroy(state->echo_state);
        state->echo_state = NULL;
    }
    gtk_label_set_text(state->status_label, "Status: Error");
}

void on_hangup_button_clicked(GtkButton *button, gpointer user_data)
{
    AppState *state = (AppState *)user_data;
    PaError err;

    if (state->ui_update_timer_id != 0)
    {
        g_source_remove(state->ui_update_timer_id);
        state->ui_update_timer_id = 0;
    }
    gtk_progress_bar_set_fraction(state->mic_level_bar,
                                  0.0);
    gtk_widget_set_visible(GTK_WIDGET(state->mic_level_bar),
                           FALSE);

    if (state->timer_id != 0)
    {
        g_source_remove(state->timer_id);
        state->timer_id = 0;
    }
    gtk_widget_set_visible(GTK_WIDGET(state->timer_label), FALSE);
    pthread_mutex_lock(&state->mutex);
    state->is_running = FALSE;
    pthread_mutex_unlock(&state->mutex);
    shutdown(state->recv_sock, SHUT_RDWR);
    close(state->send_sock);
    close(state->recv_sock);
    err = Pa_StopStream(state->stream);
    if (err != paNoError)
        fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
    err = Pa_CloseStream(state->stream);
    if (err != paNoError)
        fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
    Pa_Terminate();
    state->stream = NULL;
    speex_echo_state_destroy(state->echo_state);
    state->echo_state = NULL;
    rb_destroy(&state->send_rb);
    jitter_buffer_destroy(&state->jitter_buffer);
    gtk_label_set_text(state->status_label, "Status: Disconnected");
    gtk_widget_set_sensitive(state->call_button, TRUE);
    gtk_widget_set_sensitive(state->hangup_button, FALSE);
    gtk_widget_set_sensitive(state->mute_button, FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(state->gain_slider), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(state->threshold_slider), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->mute_button), FALSE);
    printf("[INFO] Call ended.\n");
}

static void activate(GtkApplication *app, gpointer user_data)
{
    AppState *state = (AppState *)user_data;
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "VoIP Phone (AEC + Volmeter)");
    gtk_window_set_default_size(GTK_WINDOW(window), 400, 420);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_start(grid, 10);
    gtk_widget_set_margin_end(grid, 10);
    gtk_widget_set_margin_top(grid, 10);
    gtk_widget_set_margin_bottom(grid, 10);
    GtkWidget *top_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    state->status_label = GTK_LABEL(gtk_label_new("Status: Disconnected"));
    state->timer_label = GTK_LABEL(gtk_label_new("Time: 00:00"));
    gtk_widget_set_halign(GTK_WIDGET(state->timer_label), GTK_ALIGN_END);
    gtk_widget_set_hexpand(GTK_WIDGET(state->timer_label), TRUE);
    gtk_box_append(GTK_BOX(top_box), GTK_WIDGET(state->status_label));
    gtk_box_append(GTK_BOX(top_box), GTK_WIDGET(state->timer_label));
    gtk_widget_set_visible(GTK_WIDGET(state->timer_label), FALSE);
    GtkWidget *peer_ip_label = gtk_label_new("Peer IP:");
    gtk_widget_set_halign(peer_ip_label, GTK_ALIGN_END);
    state->peer_ip_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->peer_ip_entry, "127.0.0.1");
    GtkWidget *peer_port_label = gtk_label_new("Peer Port:");
    gtk_widget_set_halign(peer_port_label, GTK_ALIGN_END);
    state->peer_port_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->peer_port_entry, "6000");
    GtkWidget *local_port_label = gtk_label_new("Local Port:");
    gtk_widget_set_halign(local_port_label, GTK_ALIGN_END);
    state->local_port_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->local_port_entry, "5000");
    GtkWidget *gain_label = gtk_label_new("Gain:");
    gtk_widget_set_halign(gain_label, GTK_ALIGN_END);
    state->gain_slider = GTK_SCALE(
        gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1.0, 5.0, 0.1));
    gtk_range_set_value(GTK_RANGE(state->gain_slider), state->gain_factor);
    GtkWidget *threshold_label = gtk_label_new("Noise Gate:");
    gtk_widget_set_halign(threshold_label, GTK_ALIGN_END);
    state->threshold_slider = GTK_SCALE(gtk_scale_new_with_range(
        GTK_ORIENTATION_HORIZONTAL, 0.0, 1000.0, 10.0));
    gtk_range_set_value(GTK_RANGE(state->threshold_slider),
                        state->noise_gate_threshold);
    gtk_widget_set_sensitive(GTK_WIDGET(state->gain_slider), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(state->threshold_slider), FALSE);

    GtkWidget *mic_level_label = gtk_label_new("Mic Level:");
    gtk_widget_set_halign(mic_level_label, GTK_ALIGN_END);
    state->mic_level_bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_widget_set_visible(GTK_WIDGET(state->mic_level_bar),
                           FALSE);

    state->call_button = gtk_button_new_with_label("Call");
    state->hangup_button = gtk_button_new_with_label("Hang Up");
    state->mute_button = gtk_toggle_button_new_with_label("Mute");
    gtk_widget_set_sensitive(state->hangup_button, FALSE);
    gtk_widget_set_sensitive(state->mute_button, FALSE);

    int row = 0;
    gtk_grid_attach(GTK_GRID(grid), top_box, 0, row++, 3, 1);
    gtk_grid_attach(GTK_GRID(grid), peer_ip_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(state->peer_ip_entry), 1, row++,
                    2, 1);
    gtk_grid_attach(GTK_GRID(grid), peer_port_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(state->peer_port_entry), 1,
                    row++, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), local_port_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(state->local_port_entry), 1,
                    row++, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), gain_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(state->gain_slider), 1, row++, 2,
                    1);
    gtk_grid_attach(GTK_GRID(grid), threshold_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(state->threshold_slider), 1,
                    row++, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), mic_level_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(state->mic_level_bar), 1, row++,
                    2, 1);
    gtk_grid_attach(GTK_GRID(grid), state->call_button, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), state->mute_button, 1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), state->hangup_button, 2, row++, 1, 1);

    gtk_window_set_child(GTK_WINDOW(window), grid);
    g_signal_connect(state->call_button, "clicked",
                     G_CALLBACK(on_call_button_clicked), state);
    g_signal_connect(state->hangup_button, "clicked",
                     G_CALLBACK(on_hangup_button_clicked), state);
    g_signal_connect(state->mute_button, "toggled",
                     G_CALLBACK(on_mute_button_toggled), state);
    g_signal_connect(state->gain_slider, "value-changed",
                     G_CALLBACK(on_gain_slider_changed), state);
    g_signal_connect(state->threshold_slider, "value-changed",
                     G_CALLBACK(on_threshold_slider_changed), state);
    gtk_widget_show(window);
}

int main(int argc, char *argv[])
{
    AppState state = {0};
    pthread_mutex_init(&state.mutex, NULL);
    state.gain_factor = 1.2;
    state.noise_gate_threshold = 150.0f;
    state.timer_id = 0;
    state.timer_started = FALSE;
    state.ui_update_timer_id = 0;
    GtkApplication *app = gtk_application_new(
        "com.example.phonegui.pa.volmeter", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &state);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    pthread_mutex_destroy(&state.mutex);
    g_free(state.peer_ip);
    return status;
}
