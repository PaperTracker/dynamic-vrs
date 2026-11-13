# PaperTracker gaze export protocol

The layer consumes OSC 1.0 messages over loopback UDP. This reference describes the wire format implemented in [gaze_packet.hpp](../src/common/gaze_packet.hpp), subscription behavior in [gaze_udp_receiver.hpp](../src/common/gaze_udp_receiver.hpp) and stabilization in [gaze_tracking.cpp](../src/layer/vrs/gaze_tracking.cpp).

## Subscription

PaperTracker listens on `127.0.0.1:gazeExportPort`, with port `9945` as the default. The layer binds an ephemeral loopback port and sends argument-free OSC messages to it.

| OSC address | Behavior |
| --- | --- |
| `/papertracker/gaze/v1/subscribe` | Subscribe immediately and renew every second. PaperTracker's lease is three seconds. |
| `/papertracker/gaze/v1/unsubscribe` | Remove the subscriber when the receiver closes. |

Replies go to the subscriber's source port. PaperTracker supports up to 16 simultaneous subscribers. The receiver accepts datagrams only from the configured loopback address and export port.

## Data packet

Every packet is exactly 72 bytes. OSC strings are zero-terminated and zero-padded to a four-byte boundary.

| Offset | Length | Content |
| --- | --- | --- |
| 0 | 24 bytes | OSC address `/papertracker/gaze/v1` and padding. |
| 24 | 12 bytes | Type tag string `,iiiffffff` and padding. |
| 36 | 36 bytes | Nine big-endian four-byte arguments. |

| Argument | Type | Layer behavior |
| --- | --- | --- |
| `sequence` | int32 bit pattern | Used to detect fresh samples, update gaze-rate metrics and track the sample applied to a rate image. PaperTracker advances it for each fresh publication. |
| `flags` | int32 bit field | Bits 0, 1 and 2 mark left, right and fused gaze as valid. Bits 3 through 5 mark measured samples and are retained without affecting projection. |
| `sample_age_us` | int32 | Decoded for protocol compatibility. Stabilizer freshness uses locally observed sequence changes. |
| `left_yaw`, `left_pitch` | float32 | Left-eye point. |
| `right_yaw`, `right_pitch` | float32 | Right-eye point. |
| `fused_yaw`, `fused_pitch` | float32 | Shared point used when `vrsGazeCombine=1` and the fused validity bit is set. |

The decoder rejects a different length or header. All six angles must be finite and strictly between -90 and +90 degrees, including angles for an invalid eye.

## Coordinates

Angles originate at the head center, with yaw positive to the right and pitch positive upward. They identify a point on a calibration plane 1.6 meters in front of the head.

```text
P = 1.6 * (tan(yaw), tan(pitch), -1)
head axes = (+X right, +Y up, -Z forward)
```

Angles are converted from degrees before taking their tangents. The layer applies `gazeFlipX`, `gazeFlipY`, `gazeForwardYawDegrees` and `gazeForwardPitchDegrees`, transforms the point through each eye's inverse eye-to-head transform, and projects it into that eye's viewport. Translation accounts for eye separation.

OpenVR supplies projection and eye transforms through `IVRSystem`. OpenXR supplies FOV and poses through `xrLocateViews`, with a view reference space located at the same display time to obtain eye-to-head transforms.

With `vrsGazeCombine=1`, a valid fused point drives both eyes. Each eye still projects it from its own position. The stabilizer handles an invalid eye using its last valid center, temporary widening and eventual drift to head-forward. An eye with no valid history starts at head-forward.

## Freshness and failure behavior

The UDP receiver keeps the latest accepted packet between polls. The stabilizer separately evaluates its freshness.

- A new sequence number counts as a fresh sample. Repeating a sequence leaves the existing freshness timestamp in place.
- By default, `gazeStaleMs=300` marks a sequence that has stopped advancing as stale after 300 ms. The rings widen, and `gazeLostMs=1000` controls the delay before a previously tracked center drifts toward head-forward.
- Setting `gazeStaleMs=0` permits a valid stationary packet to remain usable until PaperTracker changes validity or disconnects.
- With the default timeout, a valid stationary point is retained only while PaperTracker keeps sending samples with advancing sequence numbers.
- When PaperTracker closes its port, a subsequent subscription renewal can receive ICMP port-unreachable feedback. Windows reports `WSAECONNRESET`, and the receiver clears its latest packet. If PaperTracker hangs with its port open, the configured stale timeout governs the retained sample.
- The receiver is nonblocking, reads at most 128 datagrams per poll and keeps the newest accepted packet. Socket-open failures retry once per second. A later PaperTracker restart is picked up by the subscription renewal.

The shared-memory runtime control channel is independent of this UDP gaze channel.
