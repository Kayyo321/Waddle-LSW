/*
 * Execute the unchanged extracted clvk C API entry-point bodies against small
 * ownership-aware doubles. The generated header contains the pinned vendor
 * helper and API definitions; this file only supplies their CLVK object and
 * queue/map execution boundary.
 */
#include <CL/cl.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

struct _cl_command_queue {};
struct _cl_mem {};
struct _cl_event {};

struct cvk_context {
    unsigned int identity;
};

struct cvk_command;
struct cvk_buffer;
struct cvk_image {};

static unsigned int command_allocations;
static unsigned int command_live;
static unsigned int queue_enqueues;
static unsigned int map_command_allocations;
static unsigned int map_command_destructions;
static unsigned int map_command_live;
static unsigned int event_outputs;
static cl_int map_build_status = CL_SUCCESS;
static _cl_event returned_event_storage;
static _cl_event *returned_event = &returned_event_storage;
static unsigned char mapped_byte;

struct cvk_command_queue : _cl_command_queue {
    explicit cvk_command_queue(cvk_context *ctx) : context_value(ctx) {}

    bool is_valid() const { return valid; }
    cvk_context *context() const { return context_value; }
    void retain() { ++owners; }
    void release() { --owners; }

    cl_int enqueue_command_with_deps(cvk_command *cmd, cl_uint count,
                                     const cl_event *events, cl_event *event);
    cl_int enqueue_command_with_deps(cvk_command *cmd, bool blocking,
                                     cl_uint count, const cl_event *events,
                                     cl_event *event);

    cvk_context *context_value;
    unsigned int owners = 1;
    bool valid = true;
};

struct cvk_mem : _cl_mem {
    cvk_mem(cvk_context *context_arg, size_t byte_size_arg,
            cvk_mem *parent_arg = nullptr, size_t parent_offset_arg = 0)
        : context_value(context_arg), byte_size_value(byte_size_arg),
          parent_value(parent_arg), parent_offset_value(parent_offset_arg) {
        if (parent_arg) parent_arg->retain();
    }

    ~cvk_mem() {
        if (parent_value) parent_value->release();
    }

    bool is_valid() const { return valid; }
    bool is_buffer_type() const { return buffer_type; }
    cvk_context *context() const { return context_value; }
    size_t size() const { return byte_size_value; }
    cvk_mem *parent() const { return parent_value; }
    size_t parent_offset() const { return parent_offset_value; }
    bool has_any_flag(cl_mem_flags mask) const { return (flags & mask) != 0; }
    void retain() { ++owners; }
    void release() { --owners; }

    cvk_context *context_value;
    size_t byte_size_value;
    cvk_mem *parent_value;
    size_t parent_offset_value;
    unsigned int owners = 1;
    cl_mem_flags flags = 0;
    bool valid = true;
    bool buffer_type = true;
};

struct cvk_buffer : cvk_mem {
    using cvk_mem::cvk_mem;
};

struct cvk_event : _cl_event {
    explicit cvk_event(cvk_context *ctx) : context_value(ctx) {}

    bool is_valid() const { return valid; }
    cvk_context *context() const { return context_value; }

    cvk_context *context_value;
    bool valid = true;
};

struct cvk_command {
    explicit cvk_command(cvk_command_queue *queue_arg)
        : queue_value(queue_arg) {
        queue_arg->retain();
        ++command_live;
    }
    virtual ~cvk_command() {
        queue_value->release();
        --command_live;
    }

    cvk_command_queue *queue_value;
};

struct cvk_command_buffer_host_copy : cvk_command {
    cvk_command_buffer_host_copy(cvk_command_queue *queue, cl_command_type,
                                 cvk_buffer *buffer_arg, const void *, size_t,
                                 size_t)
        : cvk_command(queue), buffer_value(buffer_arg) {
        buffer_arg->retain();
        ++command_allocations;
    }
    ~cvk_command_buffer_host_copy() override { buffer_value->release(); }

    cvk_buffer *buffer_value;
};

struct cvk_command_copy_buffer : cvk_command {
    cvk_command_copy_buffer(cvk_command_queue *queue, cl_command_type,
                            cvk_buffer *source_arg,
                            cvk_buffer *destination_arg, size_t, size_t,
                            size_t)
        : cvk_command(queue), source_value(source_arg),
          destination_value(destination_arg) {
        source_arg->retain();
        destination_arg->retain();
        ++command_allocations;
    }
    ~cvk_command_copy_buffer() override {
        destination_value->release();
        source_value->release();
    }

    cvk_buffer *source_value;
    cvk_buffer *destination_value;
};

struct cvk_command_map_buffer : cvk_command {
    cvk_command_map_buffer(cvk_command_queue *queue, cvk_buffer *buffer_arg,
                          size_t, size_t, cl_map_flags, cl_command_type,
                          cvk_image * = nullptr)
        : cvk_command(queue), buffer_value(buffer_arg) {
        buffer_arg->retain();
        ++map_command_allocations;
        ++map_command_live;
    }
    ~cvk_command_map_buffer() override {
        buffer_value->release();
        --map_command_live;
        ++map_command_destructions;
    }

    cl_int build(void **map_ptr) {
        if (map_build_status == CL_SUCCESS) *map_ptr = &mapped_byte;
        return map_build_status;
    }

    cvk_buffer *buffer_value;
};

static inline cvk_command_queue *icd_downcast(cl_command_queue queue) {
    return static_cast<cvk_command_queue *>(queue);
}

static inline cvk_mem *icd_downcast(cl_mem mem) {
    return static_cast<cvk_mem *>(mem);
}

static inline cvk_event *icd_downcast(cl_event event) {
    return static_cast<cvk_event *>(event);
}

cl_int cvk_command_queue::enqueue_command_with_deps(
    cvk_command *cmd, cl_uint, const cl_event *, cl_event *event) {
    ++queue_enqueues;
    if (event) {
        *event = returned_event;
        ++event_outputs;
    }
    delete cmd;
    return CL_SUCCESS;
}

cl_int cvk_command_queue::enqueue_command_with_deps(
    cvk_command *cmd, bool, cl_uint count, const cl_event *events,
    cl_event *event) {
    return enqueue_command_with_deps(cmd, count, events, event);
}

#define CLVK_API_CALL CL_API_CALL
#define TRACE_FUNCTION(...) ((void)0)
#define LOG_API_CALL(...) ((void)0)
#include "clvk_buffer_api_under_test.hpp"

#define Check(condition)                                                      \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__,    \
                         __LINE__, #condition);                               \
            return false;                                                      \
        }                                                                      \
    } while (0)

static void reset_activity() {
    command_allocations = 0;
    command_live = 0;
    queue_enqueues = 0;
    map_command_allocations = 0;
    map_command_destructions = 0;
    event_outputs = 0;
}

static bool unchanged(const cvk_command_queue &queue,
                      const cvk_buffer &first, const cvk_buffer &second,
                      unsigned int queue_owners, unsigned int first_owners,
                      unsigned int second_owners, cl_event observed_event,
                      cl_event expected_event) {
    return command_allocations == 0 && command_live == 0 && queue_enqueues == 0 &&
           map_command_allocations == 0 && map_command_live == 0 &&
           queue.owners == queue_owners &&
           first.owners == first_owners && second.owners == second_owners &&
           observed_event == expected_event;
}

static bool test_read_write_validation(cvk_command_queue &queue,
                                       cvk_buffer &buffer,
                                       cvk_context &other_context,
                                       cvk_event &foreign_event) {
    unsigned char host[8] = {};
    cl_event event_out = reinterpret_cast<cl_event>(&foreign_event);
    const unsigned int queue_owners = queue.owners;
    const unsigned int buffer_owners = buffer.owners;

    reset_activity();
    Check(clEnqueueReadBuffer(&queue, &buffer, CL_FALSE, 0, 0, host, 0, nullptr,
                              &event_out) == CL_INVALID_VALUE);
    Check(unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out,
                    reinterpret_cast<cl_event>(&foreign_event)));

    reset_activity();
    Check(clEnqueueReadBuffer(&queue, &buffer, CL_FALSE, 0, 1, nullptr, 0,
                              nullptr, &event_out) == CL_INVALID_VALUE);
    Check(unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out,
                    reinterpret_cast<cl_event>(&foreign_event)));

    reset_activity();
    Check(clEnqueueReadBuffer(&queue, &buffer, CL_FALSE, 8, 1, host, 0, nullptr,
                              &event_out) == CL_INVALID_VALUE);
    Check(unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out,
                    reinterpret_cast<cl_event>(&foreign_event)));

    reset_activity();
    Check(clEnqueueWriteBuffer(&queue, &buffer, CL_FALSE,
                               std::numeric_limits<size_t>::max(), 1, host, 0,
                               nullptr, &event_out) == CL_INVALID_VALUE);
    Check(unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out,
                    reinterpret_cast<cl_event>(&foreign_event)));

    reset_activity();
    Check(clEnqueueWriteBuffer(&queue, &buffer, CL_FALSE, 0, 1, nullptr, 0,
                               nullptr, &event_out) == CL_INVALID_VALUE);
    Check(unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out,
                    reinterpret_cast<cl_event>(&foreign_event)));

    reset_activity();
    buffer.flags = CL_MEM_HOST_WRITE_ONLY;
    Check(clEnqueueReadBuffer(&queue, &buffer, CL_FALSE, 0, 1, host, 0, nullptr,
                              &event_out) == CL_INVALID_OPERATION);
    Check(unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out,
                    reinterpret_cast<cl_event>(&foreign_event)));
    buffer.flags = CL_MEM_HOST_READ_ONLY;
    Check(clEnqueueWriteBuffer(&queue, &buffer, CL_FALSE, 0, 1, host, 0,
                               nullptr, &event_out) == CL_INVALID_OPERATION);
    Check(unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out,
                    reinterpret_cast<cl_event>(&foreign_event)));
    buffer.flags = 0;

    cvk_buffer wrong_context(&other_context, 8);
    reset_activity();
    Check(clEnqueueReadBuffer(&queue, &wrong_context, CL_FALSE, 0, 1, host, 0,
                              nullptr, &event_out) == CL_INVALID_CONTEXT);
    Check(unchanged(queue, wrong_context, buffer, queue_owners,
                    wrong_context.owners, buffer_owners, event_out,
                    reinterpret_cast<cl_event>(&foreign_event)));

    cl_event malformed[1] = {nullptr};
    reset_activity();
    Check(clEnqueueWriteBuffer(&queue, &wrong_context, CL_FALSE, 0, 1, host, 1,
                               malformed, &event_out) == CL_INVALID_EVENT_WAIT_LIST);
    Check(unchanged(queue, wrong_context, buffer, queue_owners,
                    wrong_context.owners, buffer_owners, event_out,
                    reinterpret_cast<cl_event>(&foreign_event)));

    cl_event foreign_list[1] = {&foreign_event};
    reset_activity();
    Check(clEnqueueReadBuffer(&queue, &buffer, CL_FALSE, 0, 1, host, 1,
                              foreign_list, &event_out) == CL_INVALID_CONTEXT);
    Check(unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out,
                    reinterpret_cast<cl_event>(&foreign_event)));

    reset_activity();
    Check(clEnqueueReadBuffer(&queue, &buffer, CL_FALSE, 7, 1, host, 0, nullptr,
                              &event_out) == CL_SUCCESS);
    Check(command_allocations == 1 && command_live == 0 && queue_enqueues == 1 &&
          map_command_allocations == 0 && queue.owners == queue_owners &&
          buffer.owners == buffer_owners && event_out == returned_event);

    event_out = reinterpret_cast<cl_event>(&foreign_event);
    reset_activity();
    Check(clEnqueueWriteBuffer(&queue, &buffer, CL_FALSE, 7, 1, host, 0,
                               nullptr, &event_out) == CL_SUCCESS);
    Check(command_allocations == 1 && command_live == 0 && queue_enqueues == 1 &&
          map_command_allocations == 0 && queue.owners == queue_owners &&
          buffer.owners == buffer_owners && event_out == returned_event);
    return true;
}

static bool test_invalid_handles_and_wait_lists(cvk_command_queue &queue,
                                                cvk_buffer &buffer) {
    unsigned char host[8] = {};
    cl_event event_out = returned_event;
    cvk_buffer invalid_buffer(queue.context(), 8);
    invalid_buffer.valid = false;
    cvk_event invalid_event(queue.context());
    invalid_event.valid = false;
    cl_event invalid_event_list[1] = {&invalid_event};
    cl_event nonempty_list[1] = {nullptr};
    const unsigned int queue_owners = queue.owners;
    const unsigned int buffer_owners = buffer.owners;
    const unsigned int invalid_buffer_owners = invalid_buffer.owners;

    reset_activity();
    Check(clEnqueueReadBuffer(nullptr, &buffer, CL_FALSE, 0, 1, host, 0, nullptr,
                              &event_out) == CL_INVALID_COMMAND_QUEUE);
    Check(unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out, returned_event));

    reset_activity();
    Check(clEnqueueWriteBuffer(&queue, nullptr, CL_FALSE, 0, 1, host, 0,
                               nullptr, &event_out) == CL_INVALID_MEM_OBJECT);
    Check(unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out, returned_event));

    reset_activity();
    Check(clEnqueueReadBuffer(&queue, &invalid_buffer, CL_FALSE, 0, 1, host, 0,
                              nullptr, &event_out) == CL_INVALID_MEM_OBJECT);
    Check(unchanged(queue, invalid_buffer, buffer, queue_owners,
                    invalid_buffer_owners, buffer_owners, event_out,
                    returned_event));

    reset_activity();
    Check(clEnqueueReadBuffer(&queue, &buffer, CL_FALSE, 0, 1, host, 1, nullptr,
                              &event_out) == CL_INVALID_EVENT_WAIT_LIST);
    Check(unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out, returned_event));

    reset_activity();
    Check(clEnqueueReadBuffer(&queue, &buffer, CL_FALSE, 0, 1, host, 0,
                              nonempty_list, &event_out) == CL_INVALID_EVENT_WAIT_LIST);
    Check(unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out, returned_event));

    reset_activity();
    Check(clEnqueueReadBuffer(&queue, &buffer, CL_FALSE, 0, 1, host, 1,
                              invalid_event_list, &event_out) == CL_INVALID_EVENT_WAIT_LIST);
    Check(unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out, returned_event));

    reset_activity();
    cl_int map_error = CL_SUCCESS;
    Check(clEnqueueMapBuffer(nullptr, &buffer, CL_FALSE, CL_MAP_READ, 0, 1, 0,
                             nullptr, &event_out, &map_error) == nullptr);
    Check(map_error == CL_INVALID_COMMAND_QUEUE && command_allocations == 0 &&
          queue_enqueues == 0 && map_command_allocations == 0 && event_out == returned_event);

    reset_activity();
    Check(clEnqueueMapBuffer(&queue, nullptr, CL_FALSE, CL_MAP_READ, 0, 1, 0,
                             nullptr, &event_out, &map_error) == nullptr);
    Check(map_error == CL_INVALID_MEM_OBJECT && command_allocations == 0 &&
          queue_enqueues == 0 && map_command_allocations == 0);
    return true;
}

static bool test_copy_ranges_and_aliases(cvk_command_queue &queue,
                                         cvk_context &context) {
    cvk_buffer root(&context, 64);
    cvk_buffer other_root(&context, 64);
    cvk_buffer left(&context, 24, &root, 0);
    cvk_buffer right(&context, 24, &root, 16);
    cvk_context other_context{3};
    cvk_buffer foreign_buffer(&other_context, 64);
    cvk_event foreign_event(&other_context);
    cl_event event_out = nullptr;
    const unsigned int queue_owners = queue.owners;
    const unsigned int root_owners = root.owners;
    const unsigned int other_root_owners = other_root.owners;
    const unsigned int left_owners = left.owners;
    const unsigned int right_owners = right.owners;

    reset_activity();
    Check(clEnqueueCopyBuffer(&queue, &root, &root, 0, 4, 8, 0, nullptr,
                              &event_out) == CL_MEM_COPY_OVERLAP);
    Check(command_allocations == 0 && command_live == 0 && queue_enqueues == 0 &&
          map_command_allocations == 0 && event_out == nullptr && queue.owners == queue_owners &&
          root.owners == root_owners);

    reset_activity();
    Check(clEnqueueCopyBuffer(&queue, &left, &right, 16, 0, 8, 0, nullptr,
                              &event_out) == CL_MEM_COPY_OVERLAP);
    Check(command_allocations == 0 && queue_enqueues == 0 && map_command_allocations == 0 &&
          event_out == nullptr && queue.owners == queue_owners &&
          root.owners == root_owners && left.owners == left_owners &&
          right.owners == right_owners);

    reset_activity();
    Check(clEnqueueCopyBuffer(&queue, &left, &right, 8, 0, 8, 0, nullptr,
                              &event_out) == CL_SUCCESS);
    Check(command_allocations == 1 && command_live == 0 && queue_enqueues == 1 &&
          map_command_allocations == 0 && event_out == returned_event &&
          queue.owners == queue_owners && root.owners == root_owners &&
          left.owners == left_owners && right.owners == right_owners);

    event_out = nullptr;
    reset_activity();
    Check(clEnqueueCopyBuffer(&queue, &root, &root, 0, 8, 0, 0, nullptr,
                              &event_out) == CL_INVALID_VALUE);
    Check(command_allocations == 0 && queue_enqueues == 0 && map_command_allocations == 0 &&
          event_out == nullptr && queue.owners == queue_owners &&
          root.owners == root_owners);

    reset_activity();
    Check(clEnqueueCopyBuffer(&queue, &root, &other_root,
                              std::numeric_limits<size_t>::max(), 0, 1, 0,
                              nullptr, &event_out) == CL_INVALID_VALUE);
    Check(command_allocations == 0 && queue_enqueues == 0 && map_command_allocations == 0 &&
          event_out == nullptr && queue.owners == queue_owners &&
          root.owners == root_owners && other_root.owners == other_root_owners);

    cvk_buffer malformed_subbuffer(&context, 8, &root, 60);
    const unsigned int root_with_malformed = root.owners;
    reset_activity();
    Check(clEnqueueCopyBuffer(&queue, &malformed_subbuffer, &other_root, 0, 0,
                              8, 0, nullptr, &event_out) == CL_INVALID_VALUE);
    Check(command_allocations == 0 && queue_enqueues == 0 && map_command_allocations == 0 &&
          event_out == nullptr && queue.owners == queue_owners &&
          root.owners == root_with_malformed &&
          other_root.owners == other_root_owners);

    reset_activity();
    Check(clEnqueueCopyBuffer(nullptr, &root, &other_root, 0, 0, 1, 0, nullptr,
                              &event_out) == CL_INVALID_COMMAND_QUEUE);
    Check(command_allocations == 0 && queue_enqueues == 0 && map_command_allocations == 0 &&
          event_out == nullptr && queue.owners == queue_owners &&
          root.owners == root_with_malformed);

    reset_activity();
    Check(clEnqueueCopyBuffer(&queue, nullptr, &other_root, 0, 0, 1, 0, nullptr,
                              &event_out) == CL_INVALID_MEM_OBJECT);
    Check(command_allocations == 0 && queue_enqueues == 0 && map_command_allocations == 0 &&
          event_out == nullptr && queue.owners == queue_owners &&
          other_root.owners == other_root_owners);

    reset_activity();
    Check(clEnqueueCopyBuffer(&queue, &root, nullptr, 0, 0, 1, 0, nullptr,
                              &event_out) == CL_INVALID_MEM_OBJECT);
    Check(command_allocations == 0 && queue_enqueues == 0 && map_command_allocations == 0 &&
          event_out == nullptr && queue.owners == queue_owners &&
          root.owners == root_with_malformed);

    reset_activity();
    Check(clEnqueueCopyBuffer(&queue, &foreign_buffer, &root, 0, 0, 1, 0,
                              nullptr, &event_out) == CL_INVALID_CONTEXT);
    Check(command_allocations == 0 && queue_enqueues == 0 && map_command_allocations == 0 &&
          event_out == nullptr && queue.owners == queue_owners &&
          root.owners == root_with_malformed);

    cl_event malformed_events[1] = {nullptr};
    reset_activity();
    Check(clEnqueueCopyBuffer(&queue, &foreign_buffer, &root, 0, 0, 1, 1,
                              malformed_events, &event_out) ==
          CL_INVALID_EVENT_WAIT_LIST);
    Check(command_allocations == 0 && queue_enqueues == 0 && map_command_allocations == 0 &&
          event_out == nullptr && queue.owners == queue_owners &&
          root.owners == root_with_malformed);

    cl_event foreign_events[1] = {&foreign_event};
    reset_activity();
    Check(clEnqueueCopyBuffer(&queue, &root, &other_root, 0, 0, 1, 1,
                              foreign_events, &event_out) == CL_INVALID_CONTEXT);
    Check(command_allocations == 0 && queue_enqueues == 0 && map_command_allocations == 0 &&
          event_out == nullptr && queue.owners == queue_owners &&
          root.owners == root_with_malformed &&
          other_root.owners == other_root_owners);

    return true;
}

static bool test_map_validation(cvk_command_queue &queue, cvk_buffer &buffer,
                                cvk_context &other_context,
                                cvk_event &foreign_event) {
    cl_event event_out = reinterpret_cast<cl_event>(&foreign_event);
    cl_int map_error = CL_SUCCESS;
    const unsigned int queue_owners = queue.owners;
    const unsigned int buffer_owners = buffer.owners;

    reset_activity();
    Check(clEnqueueMapBuffer(&queue, &buffer, CL_FALSE, CL_MAP_READ, 0, 0, 0,
                             nullptr, &event_out, &map_error) == nullptr);
    Check(map_error == CL_INVALID_VALUE &&
          unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out,
                    reinterpret_cast<cl_event>(&foreign_event)));

    reset_activity();
    Check(clEnqueueMapBuffer(&queue, &buffer, CL_FALSE, CL_MAP_READ,
                             std::numeric_limits<size_t>::max(), 1, 0, nullptr,
                             &event_out, &map_error) == nullptr);
    Check(map_error == CL_INVALID_VALUE &&
          unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out,
                    reinterpret_cast<cl_event>(&foreign_event)));

    reset_activity();
    Check(clEnqueueMapBuffer(&queue, &buffer, CL_FALSE, CL_MAP_READ, 8, 1, 0,
                             nullptr, &event_out, &map_error) == nullptr);
    Check(map_error == CL_INVALID_VALUE &&
          unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out,
                    reinterpret_cast<cl_event>(&foreign_event)));

    reset_activity();
    Check(clEnqueueMapBuffer(&queue, &buffer, CL_FALSE,
                             CL_MAP_READ | CL_MAP_WRITE_INVALIDATE_REGION, 0,
                             1, 0, nullptr, &event_out, &map_error) == nullptr);
    Check(map_error == CL_INVALID_VALUE &&
          unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out,
                    reinterpret_cast<cl_event>(&foreign_event)));

    cvk_buffer wrong_context(&other_context, 8);
    reset_activity();
    Check(clEnqueueMapBuffer(&queue, &wrong_context, CL_FALSE, CL_MAP_READ, 0,
                             1, 0, nullptr, &event_out, &map_error) == nullptr);
    Check(map_error == CL_INVALID_CONTEXT &&
          unchanged(queue, wrong_context, buffer, queue_owners,
                    wrong_context.owners, buffer_owners, event_out,
                    reinterpret_cast<cl_event>(&foreign_event)));

    cl_event foreign_list[1] = {&foreign_event};
    reset_activity();
    Check(clEnqueueMapBuffer(&queue, &buffer, CL_FALSE, CL_MAP_READ, 0, 1, 1,
                             foreign_list, &event_out, &map_error) == nullptr);
    Check(map_error == CL_INVALID_CONTEXT &&
          unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out,
                    reinterpret_cast<cl_event>(&foreign_event)));

    cl_event malformed_list[1] = {nullptr};
    reset_activity();
    Check(clEnqueueMapBuffer(&queue, &buffer, CL_FALSE, CL_MAP_READ, 0, 1, 1,
                             malformed_list, &event_out, &map_error) == nullptr);
    Check(map_error == CL_INVALID_EVENT_WAIT_LIST &&
          unchanged(queue, buffer, buffer, queue_owners, buffer_owners,
                    buffer_owners, event_out,
                    reinterpret_cast<cl_event>(&foreign_event)));

    reset_activity();
    void *mapped = clEnqueueMapBuffer(&queue, &buffer, CL_FALSE, CL_MAP_READ, 7,
                                      1, 0, nullptr, &event_out, &map_error);
    Check(mapped == &mapped_byte && map_error == CL_SUCCESS &&
          map_command_allocations == 1 && map_command_destructions == 1 &&
          map_command_live == 0 && command_allocations == 0 &&
          queue_enqueues == 1 &&
          queue.owners == queue_owners && buffer.owners == buffer_owners &&
          event_out == returned_event && event_outputs == 1);
    return true;
}

static bool test_map_build_failure_releases_command(cvk_command_queue &queue,
                                                     cvk_buffer &buffer,
                                                     cvk_event &foreign_event) {
    cl_event event_out = reinterpret_cast<cl_event>(&foreign_event);
    cl_int map_error = CL_SUCCESS;
    const unsigned int queue_owners = queue.owners;
    const unsigned int buffer_owners = buffer.owners;

    reset_activity();
    map_build_status = CL_OUT_OF_RESOURCES;
    void *mapped = clEnqueueMapBuffer(&queue, &buffer, CL_FALSE, CL_MAP_READ, 0,
                                      1, 0, nullptr, &event_out, &map_error);
    map_build_status = CL_SUCCESS;
    Check(mapped == nullptr && map_error == CL_MAP_FAILURE);
    Check(map_command_allocations == 1 && map_command_destructions == 1 &&
          map_command_live == 0 && command_live == 0);
    Check(queue_enqueues == 0 && event_outputs == 0 &&
          event_out == reinterpret_cast<cl_event>(&foreign_event));
    Check(queue.owners == queue_owners && buffer.owners == buffer_owners);

    // A successful build is the control: enqueue owns and destroys the command.
    reset_activity();
    mapped = clEnqueueMapBuffer(&queue, &buffer, CL_FALSE, CL_MAP_READ, 0, 1, 0,
                                nullptr, &event_out, &map_error);
    Check(mapped == &mapped_byte && map_error == CL_SUCCESS);
    Check(map_command_allocations == 1 && map_command_destructions == 1 &&
          map_command_live == 0 && command_live == 0);
    Check(queue_enqueues == 1 && event_outputs == 1 &&
          event_out == returned_event);
    Check(queue.owners == queue_owners && buffer.owners == buffer_owners);
    return true;
}

int main() {
    cvk_context context{1};
    cvk_context other_context{2};
    cvk_command_queue queue(&context);
    cvk_buffer buffer(&context, 8);
    cvk_event foreign_event(&other_context);

    if (!test_read_write_validation(queue, buffer, other_context, foreign_event) ||
        !test_invalid_handles_and_wait_lists(queue, buffer) ||
        !test_copy_ranges_and_aliases(queue, context) ||
        !test_map_validation(queue, buffer, other_context, foreign_event) ||
        !test_map_build_failure_releases_command(queue, buffer, foreign_event)) {
        return 1;
    }
    std::puts("clvk extracted buffer API tests passed");
    return 0;
}
