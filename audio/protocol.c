#define pr_fmt(fmt) "aaudio: " fmt

#include <linux/printk.h>
#include "protocol.h"
#include "protocol_bce.h"
#include "audio.h"

int aaudio_msg_read_base(struct aaudio_msg *msg, struct aaudio_msg_base *base)
{
    if (msg->size < sizeof(struct aaudio_msg_header) + sizeof(struct aaudio_msg_base) * 2)
        return -EINVAL;
    *base = *((struct aaudio_msg_base *) ((struct aaudio_msg_header *) msg->data + 1));
    return 0;
}

/* The T2 answers SetRemoteAccess with status 6 on a cold boot. Only the
 * property readers carry a status worth acting on. */
static int aaudio_msg_verify(struct aaudio_msg *msg, u32 type, bool fatal_status)
{
    struct aaudio_msg_base *base;

    if (msg->size < sizeof(struct aaudio_msg_header) + sizeof(struct aaudio_msg_base))
        return -EINVAL;
    base = (struct aaudio_msg_base *) ((struct aaudio_msg_header *) msg->data + 1);
    if (base->msg != type)
        return -EINVAL;
    if (!base->status)
        return 0;
    if (fatal_status)
        return base->status == AAUDIO_STATUS_UNSUPPORTED ? -EOPNOTSUPP : -EREMOTEIO;
    pr_warn_ratelimited("message %u returned status %u\n", type, base->status);
    return 0;
}

#define READ_START_COMMON(type, fatal_status) \
    size_t offset = sizeof(struct aaudio_msg_header) + sizeof(struct aaudio_msg_base); (void)offset; \
    { \
        int verify = aaudio_msg_verify(msg, type, fatal_status); \
        if (verify) \
            return verify; \
    }
#define READ_START(type) READ_START_COMMON(type, false)
#define READ_START_STATUS(type) READ_START_COMMON(type, true)
#define READ_DEVID_VAR(devid) *devid = ((struct aaudio_msg_header *) msg->data)->device_id
#define READ_VAL(type) ({ offset += sizeof(type); *((type *) ((u8 *) msg->data + offset - sizeof(type))); })
#define READ_VAR(type, var) *var = READ_VAL(type)

int aaudio_msg_read_start_io_response(struct aaudio_msg *msg)
{
    READ_START(AAUDIO_MSG_START_IO_RESPONSE);
    return 0;
}

int aaudio_msg_read_stop_io_response(struct aaudio_msg *msg)
{
    READ_START(AAUDIO_MSG_STOP_IO_RESPONSE);
    return 0;
}

int aaudio_msg_read_update_timestamp(struct aaudio_msg *msg, aaudio_device_id_t *devid,
        u64 *timestamp, u64 *update_seed)
{
    READ_START(AAUDIO_MSG_UPDATE_TIMESTAMP);
    READ_DEVID_VAR(devid);
    READ_VAR(u64, timestamp);
    READ_VAR(u64, update_seed);
    return 0;
}

int aaudio_msg_read_get_property_response(struct aaudio_msg *msg, aaudio_object_id_t *obj,
        struct aaudio_prop_addr *prop, void **data, u64 *data_size)
{
    READ_START_STATUS(AAUDIO_MSG_GET_PROPERTY_RESPONSE);
    READ_VAR(aaudio_object_id_t, obj);
    READ_VAR(u32, &prop->element);
    READ_VAR(u32, &prop->scope);
    READ_VAR(u32, &prop->selector);
    READ_VAR(u64, data_size);
    *data = ((u8 *) msg->data + offset);
    /* offset += data_size; */
    return 0;
}

int aaudio_msg_read_set_property_response(struct aaudio_msg *msg, aaudio_object_id_t *obj)
{
    READ_START_STATUS(AAUDIO_MSG_SET_PROPERTY_RESPONSE);
    READ_VAR(aaudio_object_id_t, obj);
    return 0;
}

int aaudio_msg_read_property_listener_response(struct aaudio_msg *msg, aaudio_object_id_t *obj,
        struct aaudio_prop_addr *prop)
{
    READ_START(AAUDIO_MSG_PROPERTY_LISTENER_RESPONSE);
    READ_VAR(aaudio_object_id_t, obj);
    READ_VAR(u32, &prop->element);
    READ_VAR(u32, &prop->scope);
    READ_VAR(u32, &prop->selector);
    return 0;
}

int aaudio_msg_read_property_changed(struct aaudio_msg *msg, aaudio_device_id_t *devid, aaudio_object_id_t *obj,
        struct aaudio_prop_addr *prop)
{
    READ_START(AAUDIO_MSG_PROPERTY_CHANGED);
    READ_DEVID_VAR(devid);
    READ_VAR(aaudio_object_id_t, obj);
    READ_VAR(u32, &prop->element);
    READ_VAR(u32, &prop->scope);
    READ_VAR(u32, &prop->selector);
    return 0;
}

int aaudio_msg_read_set_input_stream_address_ranges_response(struct aaudio_msg *msg)
{
    READ_START(AAUDIO_MSG_SET_INPUT_STREAM_ADDRESS_RANGES_RESPONSE);
    return 0;
}

int aaudio_msg_read_get_input_stream_list_response(struct aaudio_msg *msg, aaudio_object_id_t **str_l, u64 *str_cnt)
{
    READ_START(AAUDIO_MSG_GET_INPUT_STREAM_LIST_RESPONSE);
    READ_VAR(u64, str_cnt);
    *str_l = (aaudio_device_id_t *) ((u8 *) msg->data + offset);
    /* offset += str_cnt * sizeof(aaudio_object_id_t); */
    return 0;
}

int aaudio_msg_read_get_output_stream_list_response(struct aaudio_msg *msg, aaudio_object_id_t **str_l, u64 *str_cnt)
{
    READ_START(AAUDIO_MSG_GET_OUTPUT_STREAM_LIST_RESPONSE);
    READ_VAR(u64, str_cnt);
    *str_l = (aaudio_device_id_t *) ((u8 *) msg->data + offset);
    /* offset += str_cnt * sizeof(aaudio_object_id_t); */
    return 0;
}

/* Three parallel length-prefixed u64 arrays: control ids, element ids, scope
 * ids. bridgeaudiod rejects the message unless all three counts are equal. */
int aaudio_msg_read_get_control_list_response(struct aaudio_msg *msg, aaudio_object_id_t **ctrl_l,
        u64 **elem_l, u64 **scope_l, u64 *ctrl_cnt)
{
    u64 cnt, n;
    size_t need;
    READ_START(AAUDIO_MSG_GET_CONTROL_LIST_RESPONSE);
    if (msg->size < offset + sizeof(u64))
        return -EINVAL;
    READ_VAR(u64, &cnt);
    if (cnt > (SIZE_MAX - 2 * sizeof(u64)) / (3 * sizeof(u64)))
        return -EINVAL;
    need = (size_t) cnt * 3 * sizeof(u64) + 2 * sizeof(u64);
    if (msg->size - offset < need)
        return -EINVAL;

    *ctrl_l = (aaudio_object_id_t *) ((u8 *) msg->data + offset);
    offset += cnt * sizeof(aaudio_object_id_t);
    READ_VAR(u64, &n);
    if (n != cnt)
        return -EINVAL;
    *elem_l = (u64 *) ((u8 *) msg->data + offset);
    offset += cnt * sizeof(u64);
    READ_VAR(u64, &n);
    if (n != cnt)
        return -EINVAL;
    *scope_l = (u64 *) ((u8 *) msg->data + offset);
    offset += cnt * sizeof(u64);

    *ctrl_cnt = cnt;
    return 0;
}

int aaudio_msg_read_set_remote_access_response(struct aaudio_msg *msg)
{
    READ_START(AAUDIO_MSG_SET_REMOTE_ACCESS_RESPONSE);
    return 0;
}

int aaudio_msg_read_get_device_list_response(struct aaudio_msg *msg, aaudio_device_id_t **dev_l, u64 *dev_cnt)
{
    READ_START(AAUDIO_MSG_GET_DEVICE_LIST_RESPONSE);
    READ_VAR(u64, dev_cnt);
    *dev_l = (aaudio_device_id_t *) ((u8 *) msg->data + offset);
    /* offset += dev_cnt * sizeof(aaudio_device_id_t); */
    return 0;
}

#define WRITE_START_OF_TYPE(typev, devid) \
    size_t offset = sizeof(struct aaudio_msg_header); (void) offset; \
    ((struct aaudio_msg_header *) msg->data)->type = (typev); \
    ((struct aaudio_msg_header *) msg->data)->device_id = (devid);
#define WRITE_START_COMMAND(devid) WRITE_START_OF_TYPE(AAUDIO_MSG_TYPE_COMMAND, devid)
#define WRITE_START_RESPONSE() WRITE_START_OF_TYPE(AAUDIO_MSG_TYPE_RESPONSE, 0)
#define WRITE_START_NOTIFICATION() WRITE_START_OF_TYPE(AAUDIO_MSG_TYPE_NOTIFICATION, 0)
#define WRITE_VAL(type, value) { *((type *) ((u8 *) msg->data + offset)) = value; offset += sizeof(value); }
#define WRITE_BIN(value, size) { memcpy((u8 *) msg->data + offset, value, size); offset += size; }
#define WRITE_BASE(type) WRITE_VAL(u32, type) WRITE_VAL(u32, 0)
#define WRITE_END() { msg->size = offset; }

void aaudio_msg_write_start_io(struct aaudio_msg *msg, aaudio_device_id_t dev)
{
    WRITE_START_COMMAND(dev);
    WRITE_BASE(AAUDIO_MSG_START_IO);
    WRITE_END();
}

void aaudio_msg_write_stop_io(struct aaudio_msg *msg, aaudio_device_id_t dev)
{
    WRITE_START_COMMAND(dev);
    WRITE_BASE(AAUDIO_MSG_STOP_IO);
    WRITE_END();
}

void aaudio_msg_write_get_property(struct aaudio_msg *msg, aaudio_device_id_t dev, aaudio_object_id_t obj,
        struct aaudio_prop_addr prop, void *qualifier, u64 qualifier_size)
{
    WRITE_START_COMMAND(dev);
    WRITE_BASE(AAUDIO_MSG_GET_PROPERTY);
    WRITE_VAL(aaudio_object_id_t, obj);
    WRITE_VAL(u32, prop.element);
    WRITE_VAL(u32, prop.scope);
    WRITE_VAL(u32, prop.selector);
    WRITE_VAL(u64, qualifier_size);
    WRITE_BIN(qualifier, qualifier_size);
    WRITE_END();
}

void aaudio_msg_write_set_property(struct aaudio_msg *msg, aaudio_device_id_t dev, aaudio_object_id_t obj,
        struct aaudio_prop_addr prop, void *data, u64 data_size, void *qualifier, u64 qualifier_size)
{
    WRITE_START_COMMAND(dev);
    WRITE_BASE(AAUDIO_MSG_SET_PROPERTY);
    WRITE_VAL(aaudio_object_id_t, obj);
    WRITE_VAL(u32, prop.element);
    WRITE_VAL(u32, prop.scope);
    WRITE_VAL(u32, prop.selector);
    WRITE_VAL(u64, data_size);
    WRITE_BIN(data, data_size);
    WRITE_VAL(u64, qualifier_size);
    WRITE_BIN(qualifier, qualifier_size);
    WRITE_END();
}

void aaudio_msg_write_property_listener(struct aaudio_msg *msg, aaudio_device_id_t dev, aaudio_object_id_t obj,
        struct aaudio_prop_addr prop)
{
    WRITE_START_COMMAND(dev);
    WRITE_BASE(AAUDIO_MSG_PROPERTY_LISTENER);
    WRITE_VAL(aaudio_object_id_t, obj);
    WRITE_VAL(u32, prop.element);
    WRITE_VAL(u32, prop.scope);
    WRITE_VAL(u32, prop.selector);
    WRITE_END();
}

void aaudio_msg_write_set_input_stream_address_ranges(struct aaudio_msg *msg, aaudio_device_id_t devid)
{
    WRITE_START_COMMAND(devid);
    WRITE_BASE(AAUDIO_MSG_SET_INPUT_STREAM_ADDRESS_RANGES);
    WRITE_END();
}

void aaudio_msg_write_get_input_stream_list(struct aaudio_msg *msg, aaudio_device_id_t devid)
{
    WRITE_START_COMMAND(devid);
    WRITE_BASE(AAUDIO_MSG_GET_INPUT_STREAM_LIST);
    WRITE_END();
}

void aaudio_msg_write_get_output_stream_list(struct aaudio_msg *msg, aaudio_device_id_t devid)
{
    WRITE_START_COMMAND(devid);
    WRITE_BASE(AAUDIO_MSG_GET_OUTPUT_STREAM_LIST);
    WRITE_END();
}

void aaudio_msg_write_get_control_list(struct aaudio_msg *msg, aaudio_device_id_t devid)
{
    WRITE_START_COMMAND(devid);
    WRITE_BASE(AAUDIO_MSG_GET_CONTROL_LIST);
    WRITE_END();
}

void aaudio_msg_write_set_remote_access(struct aaudio_msg *msg, u64 mode)
{
    WRITE_START_COMMAND(0);
    WRITE_BASE(AAUDIO_MSG_SET_REMOTE_ACCESS);
    WRITE_VAL(u64, mode);
    WRITE_END();
}

void aaudio_msg_write_alive_notification(struct aaudio_msg *msg, u32 proto_ver, u32 msg_ver)
{
    WRITE_START_NOTIFICATION();
    WRITE_BASE(AAUDIO_MSG_NOTIFICATION_ALIVE);
    WRITE_VAL(u32, proto_ver);
    WRITE_VAL(u32, msg_ver);
    WRITE_END();
}

void aaudio_msg_write_update_timestamp_response(struct aaudio_msg *msg)
{
    WRITE_START_RESPONSE();
    WRITE_BASE(AAUDIO_MSG_UPDATE_TIMESTAMP_RESPONSE);
    WRITE_END();
}

void aaudio_msg_write_get_device_list(struct aaudio_msg *msg)
{
    WRITE_START_COMMAND(0);
    WRITE_BASE(AAUDIO_MSG_GET_DEVICE_LIST);
    WRITE_END();
}

#define CMD_SHARED_VARS_NO_REPLY \
    int status = 0; \
    struct aaudio_send_ctx sctx;
#define CMD_SHARED_VARS \
    CMD_SHARED_VARS_NO_REPLY \
    struct aaudio_msg reply = aaudio_reply_alloc(); \
    struct aaudio_msg *buf = &reply; \
    if (!reply.data) \
        return -ENOMEM;
#define CMD_SEND_REQUEST(fn, ...) \
    if ((status = aaudio_send_cmd_sync(a, &sctx, buf, 500, fn, ##__VA_ARGS__))) \
        return status;
#define CMD_DEF_SHARED_AND_SEND(fn, ...) \
    CMD_SHARED_VARS \
    if ((status = aaudio_send_cmd_sync(a, &sctx, buf, 500, fn, ##__VA_ARGS__))) { \
        aaudio_reply_free(&reply); \
        return status; \
    }
#define CMD_DEF_SHARED_NO_REPLY_AND_SEND(fn, ...) \
    CMD_SHARED_VARS_NO_REPLY \
    CMD_SEND_REQUEST(fn, ##__VA_ARGS__);
#define CMD_HNDL_REPLY_NO_FREE(fn, ...) \
    status = fn(buf, ##__VA_ARGS__); \
    return status;
#define CMD_HNDL_REPLY_AND_FREE(fn, ...) \
    status = fn(buf, ##__VA_ARGS__); \
    aaudio_reply_free(&reply); \
    return status;

int aaudio_cmd_start_io(struct aaudio_device *a, aaudio_device_id_t devid)
{
    CMD_DEF_SHARED_AND_SEND(aaudio_msg_write_start_io, devid);
    CMD_HNDL_REPLY_AND_FREE(aaudio_msg_read_start_io_response);
}
int aaudio_cmd_stop_io(struct aaudio_device *a, aaudio_device_id_t devid)
{
    CMD_DEF_SHARED_AND_SEND(aaudio_msg_write_stop_io, devid);
    CMD_HNDL_REPLY_AND_FREE(aaudio_msg_read_stop_io_response);
}
int aaudio_cmd_get_property(struct aaudio_device *a, struct aaudio_msg *buf,
        aaudio_device_id_t devid, aaudio_object_id_t obj,
        struct aaudio_prop_addr prop, void *qualifier, u64 qualifier_size, void **data, u64 *data_size)
{
    CMD_DEF_SHARED_NO_REPLY_AND_SEND(aaudio_msg_write_get_property, devid, obj, prop, qualifier, qualifier_size);
    CMD_HNDL_REPLY_NO_FREE(aaudio_msg_read_get_property_response, &obj, &prop, data, data_size);
}
int aaudio_cmd_get_primitive_property(struct aaudio_device *a,
        aaudio_device_id_t devid, aaudio_object_id_t obj,
        struct aaudio_prop_addr prop, void *qualifier, u64 qualifier_size, void *data, u64 data_size)
{
    int status;
    struct aaudio_msg reply = aaudio_reply_alloc();
    void *r_data;
    u64 r_data_size;
    if (!reply.data)
        return -ENOMEM;
    if ((status = aaudio_cmd_get_property(a, &reply, devid, obj, prop, qualifier, qualifier_size,
            &r_data, &r_data_size)))
        goto finish;
    if (r_data_size != data_size) {
        status = -EINVAL;
        goto finish;
    }
    memcpy(data, r_data, data_size);
finish:
    aaudio_reply_free(&reply);
    return status;
}
int aaudio_cmd_set_property(struct aaudio_device *a, aaudio_device_id_t devid, aaudio_object_id_t obj,
        struct aaudio_prop_addr prop, void *qualifier, u64 qualifier_size, void *data, u64 data_size)
{
    CMD_DEF_SHARED_AND_SEND(aaudio_msg_write_set_property, devid, obj, prop, data, data_size,
            qualifier, qualifier_size);
    CMD_HNDL_REPLY_AND_FREE(aaudio_msg_read_set_property_response, &obj);
}
int aaudio_cmd_property_listener(struct aaudio_device *a, aaudio_device_id_t devid, aaudio_object_id_t obj,
        struct aaudio_prop_addr prop)
{
    CMD_DEF_SHARED_AND_SEND(aaudio_msg_write_property_listener, devid, obj, prop);
    CMD_HNDL_REPLY_AND_FREE(aaudio_msg_read_property_listener_response, &obj, &prop);
}
int aaudio_cmd_set_input_stream_address_ranges(struct aaudio_device *a, aaudio_device_id_t devid)
{
    CMD_DEF_SHARED_AND_SEND(aaudio_msg_write_set_input_stream_address_ranges, devid);
    CMD_HNDL_REPLY_AND_FREE(aaudio_msg_read_set_input_stream_address_ranges_response);
}
int aaudio_cmd_get_input_stream_list(struct aaudio_device *a, struct aaudio_msg *buf, aaudio_device_id_t devid,
        aaudio_object_id_t **str_l, u64 *str_cnt)
{
    CMD_DEF_SHARED_NO_REPLY_AND_SEND(aaudio_msg_write_get_input_stream_list, devid);
    CMD_HNDL_REPLY_NO_FREE(aaudio_msg_read_get_input_stream_list_response, str_l, str_cnt);
}
int aaudio_cmd_get_output_stream_list(struct aaudio_device *a, struct aaudio_msg *buf, aaudio_device_id_t devid,
        aaudio_object_id_t **str_l, u64 *str_cnt)
{
    CMD_DEF_SHARED_NO_REPLY_AND_SEND(aaudio_msg_write_get_output_stream_list, devid);
    CMD_HNDL_REPLY_NO_FREE(aaudio_msg_read_get_output_stream_list_response, str_l, str_cnt);
}
int aaudio_cmd_get_control_list(struct aaudio_device *a, struct aaudio_msg *buf, aaudio_device_id_t devid,
        aaudio_object_id_t **ctrl_l, u64 **elem_l, u64 **scope_l, u64 *ctrl_cnt)
{
    CMD_DEF_SHARED_NO_REPLY_AND_SEND(aaudio_msg_write_get_control_list, devid);
    CMD_HNDL_REPLY_NO_FREE(aaudio_msg_read_get_control_list_response, ctrl_l, elem_l, scope_l, ctrl_cnt);
}
int aaudio_cmd_set_remote_access(struct aaudio_device *a, u64 mode)
{
    CMD_DEF_SHARED_AND_SEND(aaudio_msg_write_set_remote_access, mode);
    CMD_HNDL_REPLY_AND_FREE(aaudio_msg_read_set_remote_access_response);
}
int aaudio_cmd_get_device_list(struct aaudio_device *a, struct aaudio_msg *buf,
        aaudio_device_id_t **dev_l, u64 *dev_cnt)
{
    CMD_DEF_SHARED_NO_REPLY_AND_SEND(aaudio_msg_write_get_device_list);
    CMD_HNDL_REPLY_NO_FREE(aaudio_msg_read_get_device_list_response, dev_l, dev_cnt);
}