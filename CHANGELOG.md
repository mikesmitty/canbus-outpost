# Changelog

## [0.3.0](https://github.com/mikesmitty/canbus-outpost/compare/v0.2.1...v0.3.0) (2026-04-10)


### Features

* **servo:** add "No Change" default state option and default to closed ([8e3acc5](https://github.com/mikesmitty/canbus-outpost/commit/8e3acc5dbc9800a2b613a01acdd93002f33e7c38))

## [0.2.1](https://github.com/mikesmitty/canbus-outpost/compare/v0.2.0...v0.2.1) (2026-04-10)


### Bug Fixes

* **lcc:** correct Update Complete command and fix flash writes ([c6c54f6](https://github.com/mikesmitty/canbus-outpost/commit/c6c54f63b11c725be7dc7756b86b1fd509bc4b51))


### Miscellaneous

* **ci:** init FreeRTOS submodule for RP2350 port ([33793cd](https://github.com/mikesmitty/canbus-outpost/commit/33793cd8935302fa61fbcb8044102c5f023625af))

## [0.2.0](https://github.com/mikesmitty/canbus-outpost/compare/v0.1.0...v0.2.0) (2026-04-09)


### Features

* add initial firmware implementation for canbus-outpost ([e8db9c5](https://github.com/mikesmitty/canbus-outpost/commit/e8db9c581d01500df24587b78b2423c6a16b2d98))
* **ci:** add release-please workflow and improve FreeRTOS inclusion ([d99bae6](https://github.com/mikesmitty/canbus-outpost/commit/d99bae697126726b3fe6cd4a7b150a80ca69e776))
* **firmware:** add identify LED driver and release-please versioning ([82a06f5](https://github.com/mikesmitty/canbus-outpost/commit/82a06f532db7e8cd5938ba43187ce10f36e5d4f7))
* **firmware:** add LCC CDI configuration and servo control enhancements ([f646966](https://github.com/mikesmitty/canbus-outpost/commit/f64696652d5826a22e10b7a03eab24ae933adb91))
* **firmware:** implement LCC servo switch controller with FreeRTOS ([eff3aef](https://github.com/mikesmitty/canbus-outpost/commit/eff3aef8f9c69e1d3cc23b4944237df86a78b728))
* initial commit ([35d93e9](https://github.com/mikesmitty/canbus-outpost/commit/35d93e993fa1c1821139c826e4c87451ff0ce4ea))
* **lcc:** broadcast switch state on startup ([2fa6b0f](https://github.com/mikesmitty/canbus-outpost/commit/2fa6b0fc0f0fe9ec978599f794ea86c657b5da2d))


### Bug Fixes

* Correct RailCom PIO timing and implement cutout-based gating ([83975ca](https://github.com/mikesmitty/canbus-outpost/commit/83975ca658c8206e8ac5d75f308612406ca71e06))
* switch from LM393 to TLV3202 for railcom signals ([46103d2](https://github.com/mikesmitty/canbus-outpost/commit/46103d206493b011c9451f2a776c54061438b2c6))
* update part rotations ([f5b980b](https://github.com/mikesmitty/canbus-outpost/commit/f5b980b29f2e95d8073f5162f05b1b61f52c5e41))


### Miscellaneous

* update .gitignore ([21451e4](https://github.com/mikesmitty/canbus-outpost/commit/21451e441d43007f07a678a31461fa850c250cec))
