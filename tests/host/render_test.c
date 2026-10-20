/* render_test - a started consumer renders every frame, with no threads.
 *
 * Usage: render_test project.mlt
 *
 * MLT's null consumer (melt -consumer null, the engine test's speed test)
 * renders nothing itself: upstream, its read-ahead thread fetches each
 * frame's picture and sound. With MLT_NO_THREADS there is no such thread,
 * so mlt_consumer_rt_frame must do it, or the null consumer "plays" a 1080p
 * project at hundreds of frames a second without decoding anything (the
 * first Pi test, 2026-10-20). Every frame shown must have its picture and
 * sound, at the consumer's size.
 */
#include "mltt.h"

static int shown = 0, with_image = 0, with_audio = 0, wrong_size = 0;

static void on_show(mlt_properties owner, mlt_consumer consumer, mlt_event_data data)
{
    (void) owner;
    mlt_frame frame = mlt_event_data_to_frame(data);
    mlt_properties p = MLT_FRAME_PROPERTIES(frame);
    shown++;
    if (mlt_properties_get_data(p, "image", NULL))
        with_image++;
    if (mlt_properties_get_data(p, "audio", NULL))
        with_audio++;
    int w = mlt_properties_get_int(p, "width"), h = mlt_properties_get_int(p, "height");
    if (w != mlt_properties_get_int(MLT_CONSUMER_PROPERTIES(consumer), "width")
        || h != mlt_properties_get_int(MLT_CONSUMER_PROPERTIES(consumer), "height"))
        wrong_size++;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: render_test project.mlt\n");
        return 2;
    }
    mltt t;
    if (!mltt_open(&t, "hdv_720_25p", "xml", argv[1]))
        return 1;
    mlt_properties cp = MLT_CONSUMER_PROPERTIES(t.consumer);
    mlt_properties_set_int(cp, "terminate_on_pause", 1);
    mlt_events_listen(cp, t.consumer, "consumer-frame-show", (mlt_listener) on_show);
    mlt_consumer_start(t.consumer); /* (no threads: returns when it has finished) */
    mlt_consumer_stop(t.consumer);
    int length = mlt_producer_get_length(t.producer);
    CHECK(shown >= length, "%d frames shown of %d", shown, length);
    CHECK(with_image == shown, "%d of %d frames had a picture", with_image, shown);
    CHECK(with_audio == shown, "%d of %d frames had sound", with_audio, shown);
    CHECK(wrong_size == 0, "%d frames not at the consumer's size", wrong_size);
    mltt_close(&t);
    printf("render_test: %s\n", mltt_failures ? "FAILED" : "passed");
    return mltt_failures != 0;
}
