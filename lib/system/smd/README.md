# LWS System Message Distribution

## Overview

Independent pieces of a system may need to become aware of events and state
changes in the other pieces quickly, along with the new state if it is small.
These messages are local to inside a system, although they may be triggered by
events outside of it.  Examples include keypresses, or networking state changes.
Individual OSes and frameworks typically have their own fragmented apis for
message-passing, but the lws apis operate the same across any platforms
including, eg, Windows and RTOS and allow crossplatform code to be written once.

Message payloads are short, less than 384 bytes, below system limits for atomic
pipe or UDS datagrams and consistent with heap usage on smaller systems, but
large enough to carry JSON usefully.  Messages are typically low duty cycle.

Messages may be sent by any registered participant, they are allocated on heap
in a linked-list, and delivered no sooner than next time around the event loop.
This retains the ability to handle multiple event queuing in one event loop trip
while guaranteeing message handling is nonrecursive.  Messages are passed to all
other registered participants before being destroyed.

`lws_smd` apis allow publication and subscription of message objects between
participants that are in a single process and are informed by callback from lws
service thread context, and, via Secure Streams proxying as the IPC method, also
between those in different processes.  Registering as a participant and sending
messages are threadsafe APIs.

## Message Class

Message class is a bitfield messages use to indicate their general type, eg,
network status, or UI event like a keypress.  Participants set a bitmask to
filter what kind of messages they care about, classes that are 0 in the peer's
filter are never delivered to the peer.   A message usually indicates it is a
single class, but it's possible to set multiple class bits and match on any.  If
so, care must be taken the payload can be parsed by readers expecting any of the
indicated classes, eg, by using JSON.

`lws_smd` tracks a global union mask for all participants' class mask.  Requests
to allocate a message of a class that no participant listens for are rejected,
not at distribution-time but at message allocation-time, so no heap or cpu is
wasted on things that are not currently interesting; but such messages start to
appear as soon as a participant appears that wants them.  The message generation
action should be bypassed without error in the case lws_smd_msg_alloc()
returns NULL.

## Messaging guarantees

Sent messages are delivered to all registered participants whose class mask
indicates they want it, including the sender.  The send apis are threadsafe.

Locally-delivered message delivery callbacks occur from lws event loop thread
context 0 (the only one in the default case LWS_MAX_SMP = 1).  Clients in
different processes receive callbacks from the thread context of their UDS
networking thread.

The message payload may be destroyed immediately when you return from the
callback, you can't store references to it or expect it to be there later.

Messages are timestamped with a systemwide monotonic timestamp.  When
participants are on the lws event loop, messages are delivered in-order.  When
participants are on different threads, delivery order depends on platform lock
acquisition.  External process participants are connected by the Unix Domain
Socket capability of Secure Streams, and may be delivered out-of-order;
receivers that care must consult the message creation timestamps.

## Message Refcounting

To avoid keeping a list of the length of the number of participants for each
message, a refcount is used in the message, computed at the time the message
arrived considering the number of active participants that indicated a desire to
receive messages of that class.

Since peers may detach / close their link asynchronously, the logical peer
objects at the distributor defer destroying themselves until there is no more
possibility of messages arriving timestamped with the period they were active.
A grace period (default 2s) is used to ensure departing peers correctly account
for message refcounts before being destroyed.

## Message creation

Messages may contain arbitrary text or binary data depending on the class.  JSON
is recommended since lws_smd messages are small and low duty cycle but have
open-ended content: JSON is maintainable, extensible, debuggable and self-
documenting and avoids, eg, fragile dependencies on header versions shared
between teams.  To simplify issuing JSON, a threadsafe api to create and send
messages in one step using format strings is provided:

```
int
lws_smd_msg_printf(struct lws_context *ctx, lws_smd_class_t _class,
		   const char *format, ...);
```

## Secure Streams `lws_smd` streamtype

When built with LWS_WITH_SECURE_STREAMS, lws_smd exposes a built-in streamtype
`_lws_smd` which user Secure Streams may use to interoperate with lws_smd using
SS payload semantics.

When using `_lws_smd`, the SS info struct member `manual_initial_tx_credit`
provided by the user when creating the Secure Stream is overloaded to be used as
the RX class mask for the SMD connection associated with the Secure Stream.

Both RX and TX payloads have a 16-byte binary header before the actual payload.
For TX, although the header is 16-bytes, only the first 64-bit class bitfield
needs setting, the timestamp is fetched and added by lws.

 - MSB-first 64-bit class bitfield (currently only 32 least-sig in use) 
 - MSB-First Order 64-bit us-resolution timestamp
 