# Third-party notices

This project interoperates with REFramework and uses its public plugin API:

- REFramework, Copyright (c) 2019 praydog, MIT License (as in the bundled license).
- https://github.com/praydog/REFramework
- Full license: `REFramework-LICENSE.txt`.

DualSense adaptive-trigger packet encoding was derived from the MIT-licensed
effect factory reference published by John "Nielk1" Klein:

- https://gist.github.com/Nielk1/6d54cc2c00d2201ccb8c2720ad7538db
- Copyright (c) 2021-2022 John "Nielk1" Klein. Full license:
  `Nielk1-Trigger-Effects-LICENSE.txt`.

Public protocol behavior was cross-checked against these reference
implementations/documentation; their source code is not bundled:

- SDL PS5 HIDAPI implementation: https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_ps5.c
- SDL DualSense speaker-route test implementation: https://github.com/libsdl-org/SDL/blob/main/test/testcontroller.c
- Microsoft GameInput DualSense companion sample: https://github.com/microsoftconnect/GameInput/tree/main/companion/DualSense

The locally installed game's Wwise haptics bank was inspected with wwiser and
decoded with vgmstream. Neither tool is bundled:

- wwiser: https://github.com/bnnm/wwiser
- vgmstream: https://github.com/vgmstream/vgmstream

The setup wizard uses the MIT-licensed Inno Setup Simplified Chinese
translation maintained by Zhenghan Yang (Kira), pinned from commit
`1ff90acc4ed4aee82b1cda43253243deee3daed4`. Its license is included as
`Inno-Chinese-Translation-LICENSE.txt`.

The packaged haptics and gauntlet-voice WAV assets are not covered by the
REFramework MIT license or the licenses of the tools used to inspect or decode
them. No separate authorization to redistribute game-derived audio or other
third-party assets has been verified in the release records reviewed for this
project. This notice does not assert that the publisher has obtained such
authorization. Bundling an asset, publishing source code, or stating a learning
or research purpose does not establish redistribution permission. The relevant
asset rights must be checked separately; third-party trademarks and other
rights remain with their respective holders.

中文说明：第三方代码和工具按各自许可证使用。本项目未核实游戏派生音频等
第三方素材的单独再分发授权，本说明不声明发布者已经取得此类授权。
素材随包提供、源码公开或注明学习研究用途，均不能证明已取得再分发许可。
相关素材权利须单独核实，第三方商标及其他权利仍归各自权利人所有。
