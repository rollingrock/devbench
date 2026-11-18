# devbench

devbench exposes a running game to AI agents and scripts so they can inspect it and drive it, including standing in for the VR player's headset and controllers in end-to-end tests.

## VR input

**Controller**:
A tracked VR hand device as OpenVR reports it: a pose plus button, touch and axis state. devbench drives Controllers.
_Avoid_: wand, hand (for the device)

**Hand**:
The in-game body part that follows a Controller, placed by the game or by a body mod such as FRIK.
_Avoid_: controller (for the body part)

**Wand**:
The in-game node the game places at a Controller's pose each frame, present with or without a body mod.

**Handedness**:
Which Controller is the player's primary one: the right unless the game is set left-handed. Changing it swaps which Controller each of the game's two Wands follows.

**Primary / offhand**:
A Controller named by its role for the Handedness instead of physically (left / right), resolved when a request arrives.
_Avoid_: main hand, primaryHand

**Tracked set**:
The HMD and both Controllers taken together, the unit devbench replaces as a whole.
_Avoid_: devices, rig

**Frame**:
One timestamped snapshot of the Tracked set: three poses plus both Controllers' button, touch and axis state.

**Sequence**:
A bounded, timed list of Frames that devbench plays in place of the physical Tracked set, held by a single owner until it ends or is stopped.
_Avoid_: recording, replay (those name a separate feature)

**Reach**:
Skyrim's way of placing the hands: calibrating how a tracking-space move shows up at the hand bones, then driving the Controllers until the hands reach a target relative to a named node, correcting against where they actually land. Fallout 4 VR uses Pose instead.
_Avoid_: move hand

**Pose**:
Placing and holding the Tracked set without calibrating: the head at one eye position and turned (pitch and yaw, never moved), each Controller put where its Wand lands on a node or world point, mapped through the HMD node and a turn fitted from the held Wands.
_Avoid_: reach (which calibrates), stance

**Ready pose**:
The Pose a held Tracked set starts in and returns to: the head pitched down and both hands in front at chest height, in view.

**Head offset**:
A Pose world target moved along the head's axes as posed (outward, forward, up), outward being toward the placed hand's own side, so one offset puts either hand at its mirror image around the head.

**Inward yaw**:
A Controller's yaw given as a turn toward the body, the same for either hand, so one recorded aim fits the left and the right Controller. It is in tracking space, so the hands mirror around the body's forward, not the head's.
_Avoid_: mirrored yaw
