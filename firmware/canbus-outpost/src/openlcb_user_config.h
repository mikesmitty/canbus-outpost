#ifndef __OPENLCB_USER_CONFIG__
#define __OPENLCB_USER_CONFIG__

// Train command station feature set
#define OPENLCB_COMPILE_EVENTS
#define OPENLCB_COMPILE_DATAGRAMS
#define OPENLCB_COMPILE_MEMORY_CONFIGURATION
#define OPENLCB_COMPILE_TRAIN
#define OPENLCB_COMPILE_TRAIN_SEARCH

// Message buffer pool
#define USER_DEFINED_BASIC_BUFFER_DEPTH              32
#define USER_DEFINED_DATAGRAM_BUFFER_DEPTH           4
#define USER_DEFINED_SNIP_BUFFER_DEPTH               4
#define USER_DEFINED_STREAM_BUFFER_DEPTH             1
#define USER_DEFINED_STREAM_BUFFER_LEN               256

// Virtual node slots: 1 command station + up to 20 train nodes
#define USER_DEFINED_NODE_BUFFER_DEPTH               21

// Events
#define USER_DEFINED_PRODUCER_COUNT                  8
#define USER_DEFINED_PRODUCER_RANGE_COUNT            2
#define USER_DEFINED_CONSUMER_COUNT                  8
#define USER_DEFINED_CONSUMER_RANGE_COUNT            2

// Train protocol
#define USER_DEFINED_TRAIN_NODE_COUNT                20
#define USER_DEFINED_MAX_LISTENERS_PER_TRAIN         6
#define USER_DEFINED_MAX_TRAIN_FUNCTIONS             29

// Listener alias verification
#define USER_DEFINED_LISTENER_PROBE_TICK_INTERVAL    1
#define USER_DEFINED_LISTENER_PROBE_INTERVAL_TICKS   250
#define USER_DEFINED_LISTENER_VERIFY_TIMEOUT_TICKS   30

#ifdef __cplusplus
extern "C" {
#endif

extern const struct node_parameters_TAG OpenLcbUserConfig_node_parameters;
extern const struct node_parameters_TAG OpenLcbUserConfig_train_node_parameters;

#ifdef __cplusplus
}
#endif

#endif // __OPENLCB_USER_CONFIG__
