#include "ringbuffer.h"
#include <string.h>
#include <stdlib.h>

int ringbuffer_init(ringbuffer_t *rb) {
    if (!rb) return -1;
    
    // Allocate the 10MB buffer
    rb->pulled_data = (uint8_t *)malloc(RINGBUFFER_SIZE);
    if (!rb->pulled_data) return -1;
    
    rb->read_offset = 0;
    rb->write_offset = 0;
    
    return 0;
}

void ringbuffer_free(ringbuffer_t *rb) {
    if (rb && rb->pulled_data) {
        free(rb->pulled_data);
        rb->pulled_data = NULL;
    }
}

size_t ringbuffer_available_write(const ringbuffer_t *rb) {
    if (!rb || !rb->pulled_data) return 0;
    
    // Calculate available space
    if (rb->write_offset >= rb->read_offset) {
        // Write pointer is ahead, space wraps around
        return RINGBUFFER_SIZE - rb->write_offset + rb->read_offset - 1;
    } else {
        // Read pointer ahead, space is in between
        return rb->read_offset - rb->write_offset - 1;
    }
}

size_t ringbuffer_available_read(const ringbuffer_t *rb) {
    if (!rb || !rb->pulled_data) return 0;
    
    // Calculate available data
    if (rb->write_offset >= rb->read_offset) {
        return rb->write_offset - rb->read_offset;
    } else {
        // Wrapped around
        return RINGBUFFER_SIZE - rb->read_offset + rb->write_offset;
    }
}

void ringbuffer_get_write_ptr(ringbuffer_t *rb, uint8_t **data, size_t *len) {
    if (!rb || !rb->pulled_data || !data || !len) {
        if (data) *data = NULL;
        if (len) *len = 0;
        return;
    }
    
    size_t available = ringbuffer_available_write(rb);
    
    if (available == 0) {
        *data = NULL;
        *len = 0;
        return;
    }
    
    // Always return contiguous space to avoid wrapping
    *data = &rb->pulled_data[rb->write_offset];
    
    // Check if we can write to the end without wrapping
    size_t space_to_end = RINGBUFFER_SIZE - rb->write_offset;
    
    if (rb->write_offset >= rb->read_offset) {
        // Write pointer ahead, can write to end but leave 1 byte buffer
        // to distinguish full from empty
        if (space_to_end > 1) {
            *len = space_to_end - 1;
            if (*len > available) *len = available;
        } else {
            *len = 0;
        }
    } else {
        // Read pointer ahead, can write up to read pointer
        *len = rb->read_offset - rb->write_offset - 1;
        if (*len > available) *len = available;
    }
}

void ringbuffer_commit_write(ringbuffer_t *rb, size_t len) {
    if (!rb || len == 0) return;
    
    size_t available = ringbuffer_available_write(rb);
    if (len > available) len = available;
    
    rb->write_offset = (rb->write_offset + len) % RINGBUFFER_SIZE;
}

void ringbuffer_next_read(ringbuffer_t *rb, uint8_t **data, size_t *len) {
    if (!rb || !rb->pulled_data || !data || !len) {
        if (data) *data = NULL;
        if (len) *len = 0;
        return;
    }
    
    size_t available = ringbuffer_available_read(rb);
    
    if (available == 0) {
        *data = NULL;
        *len = 0;
        return;
    }
    
    // Return pointer to readable data
    *data = &rb->pulled_data[rb->read_offset];
    
    // Check if data wraps around
    if (rb->write_offset >= rb->read_offset) {
        // Contiguous data
        *len = rb->write_offset - rb->read_offset;
    } else {
        // Wrapped around - return first contiguous chunk
        *len = RINGBUFFER_SIZE - rb->read_offset;
    }
    
    if (*len > available) *len = available;
}

void ringbuffer_peek_read(const ringbuffer_t *rb, uint8_t **data, size_t *len) {
    if (!rb || !rb->pulled_data || !data || !len) {
        if (data) *data = NULL;
        if (len) *len = 0;
        return;
    }
    
    size_t available = ringbuffer_available_read(rb);
    
    if (available == 0) {
        *data = NULL;
        *len = 0;
        return;
    }
    
    // Return pointer to readable data (const version, doesn't modify rb)
    *data = (uint8_t *)&rb->pulled_data[rb->read_offset];
    
    // Check if data wraps around
    if (rb->write_offset >= rb->read_offset) {
        // Contiguous data
        *len = rb->write_offset - rb->read_offset;
    } else {
        // Wrapped around - return first contiguous chunk
        *len = RINGBUFFER_SIZE - rb->read_offset;
    }
    
    if (*len > available) *len = available;
}

void ringbuffer_advance_read(ringbuffer_t *rb, size_t len) {
    if (!rb || !rb->pulled_data || len == 0) return;
    
    size_t available = ringbuffer_available_read(rb);
    if (len > available) len = available;
    
    rb->read_offset = (rb->read_offset + len) % RINGBUFFER_SIZE;
}

size_t ringbuffer_write(ringbuffer_t *rb, const uint8_t *data, size_t len) {
    if (!rb || !rb->pulled_data || !data || len == 0) return 0;
    
    size_t total_written = 0;
    
    while (len > 0) {
        uint8_t *write_ptr = NULL;
        size_t write_len = 0;
        
        ringbuffer_get_write_ptr(rb, &write_ptr, &write_len);
        
        if (write_len == 0) break;
        
        if (write_len > len) write_len = len;
        
        memcpy(write_ptr, data, write_len);
        ringbuffer_commit_write(rb, write_len);
        
        total_written += write_len;
        data += write_len;
        len -= write_len;
    }
    
    return total_written;
}

size_t ringbuffer_read(ringbuffer_t *rb, uint8_t *data, size_t len) {
    if (!rb || !rb->pulled_data || !data || len == 0) return 0;
    
    size_t total_read = 0;
    
    while (len > 0) {
        uint8_t *read_ptr = NULL;
        size_t read_len = 0;
        
        ringbuffer_next_read(rb, &read_ptr, &read_len);
        
        if (read_len == 0) break;
        
        if (read_len > len) read_len = len;
        
        memcpy(data, read_ptr, read_len);
        ringbuffer_advance_read(rb, read_len);
        
        total_read += read_len;
        data += read_len;
        len -= read_len;
    }
    
    return total_read;
}
