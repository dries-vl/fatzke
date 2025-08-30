struct data
{
    u32* src;
    u32 sw, sh;
    u32* dst;
    u32 dw; // display width
    u32 outw; // todo: do we need this, isn't this just sw * 2?
};

struct thread_data
{
    struct scaler* scaler;
    int index;
};

struct scaler
{
    int number_of_threads;
    thread threads[8];
    barrier barrier;
    struct thread_data thread_data[8]; // per-thread context: pointer to (this) scaler and index
    struct data data;
};

void* thread_loop(void* thread_args)
{
    struct thread_data* context = (struct thread_data*)thread_args;
    struct scaler* scaler = context->scaler;
    const int worker_index = context->index;

    for (;;)
    {
        barrier_wait(&scaler->barrier); // wait until main calls scale()

        u32 total_rows = scaler->data.sh;
        u32 rows_per_worker = total_rows / (u32)scaler->number_of_threads; // divide rows among threads
        u32 remainder_rows = total_rows % (u32)scaler->number_of_threads;

        u32 y_begin = worker_index * rows_per_worker + (worker_index < remainder_rows ? worker_index : remainder_rows);
        u32 y_end = y_begin + rows_per_worker + (worker_index < remainder_rows ? 1u : 0u); // add remainder rows too

        for (u32 y = y_begin; y < y_end; ++y)
        {
            u32* source_row = scaler->data.src + y * scaler->data.sw;
            u32* dest_row0 = scaler->data.dst + (y * 2) * scaler->data.dw;
            u32* dest_row1 = dest_row0 + scaler->data.dw;
            u32 pair_count = scaler->data.dw >> 1;

            for (u32 x = 0; x < pair_count; ++x)
            {
                u32 pixel = source_row[x];
                u64 packed = (u64)pixel | ((u64)pixel << 32);
                ((u64*)dest_row0)[x] = packed;
                ((u64*)dest_row1)[x] = packed;
            }

            if (scaler->data.dw & 1u)
            {
                dest_row0[scaler->data.dw - 1] = source_row[pair_count];
                dest_row1[scaler->data.dw - 1] = source_row[pair_count];
            }
        }

        barrier_wait(&scaler->barrier);
    }
    return NULL;
}

void create_scaler(struct scaler* scaler, int number_of_threads)
{
    *scaler = (struct scaler){0};
    scaler->number_of_threads = number_of_threads;
    barrier_init(&scaler->barrier, (unsigned)(scaler->number_of_threads + 1));

    for (int i = 0; i < scaler->number_of_threads; ++i)
    {
        scaler->thread_data[i].scaler = scaler;
        scaler->thread_data[i].index = i;
        thread_create(&scaler->threads[i], thread_loop, &scaler->thread_data[i]);
    }
}

void scale(struct scaler* scaler, u32* src, u32 sw, u32 sh, u32* dst, u32 dw)
{
    scaler->data.src = src;
    scaler->data.dst = dst;
    scaler->data.sw = sw;
    scaler->data.sh = sh;
    scaler->data.dw = dw;
    scaler->data.outw = sw * 2;

    barrier_wait(&scaler->barrier); // start the threads
    barrier_wait(&scaler->barrier); // wait for the threads to finish
}
