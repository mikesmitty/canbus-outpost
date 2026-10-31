# Changelog

## [0.4.0](https://github.com/mikesmitty/canbus-outpost/compare/v0.3.1...v0.4.0) (2026-10-31)


### Features

* add SSSB LED signal header ([7316e79](https://github.com/mikesmitty/canbus-outpost/commit/7316e792a6361132b21dae134f69fe599de6d2f2))
* add usb jack for flashing ([23d9043](https://github.com/mikesmitty/canbus-outpost/commit/23d90432513d269713aefd2f7569cba6d5ddb66d))
* **lcc:** deliver a node's own events to its consumers ([02539ef](https://github.com/mikesmitty/canbus-outpost/commit/02539ef0d16baf1de0bb59c8b8443e417c0d4972))
* **lcc:** send identify-all in batches ([5d83608](https://github.com/mikesmitty/canbus-outpost/commit/5d8360899c65e68231a0d96f02e7023ff2765e72))
* **signals:** drive SSSB signal heads over LCC ([ef617c2](https://github.com/mikesmitty/canbus-outpost/commit/ef617c2d49f9a6221587862ece0188bf25f898b7))


### Bug Fixes

* **can:** resend a transmit halted by can2040's rx stall recovery ([e703e1a](https://github.com/mikesmitty/canbus-outpost/commit/e703e1a4a694d5f356e9f48bf00292301737c63a))
* **cdi:** make the CDI valid against its schema ([8da3ca3](https://github.com/mikesmitty/canbus-outpost/commit/8da3ca34fb4a41bbe5638742e0636c0c4779c433))
* **config:** save configuration writes without waiting for Update Complete ([412ac87](https://github.com/mikesmitty/canbus-outpost/commit/412ac87ebcb00482a786aed9dc438a550903c3b3))
* **servo:** stop the PWM after the hold time on instant moves ([ced84f3](https://github.com/mikesmitty/canbus-outpost/commit/ced84f3578f8423de35255f0a2695a9cc289a577))
* **usb:** fit the longest console messages ([6016a2a](https://github.com/mikesmitty/canbus-outpost/commit/6016a2aa2dadc6c216ca4d34535ec41a7749286f))


### Miscellaneous

* footprint path bugfix ([b6df4ce](https://github.com/mikesmitty/canbus-outpost/commit/b6df4ce17cfd1114fda6938ffe484d931bfac0db))


### Documentation

* **readme:** describe the board and its signal heads ([13e641d](https://github.com/mikesmitty/canbus-outpost/commit/13e641d67533051b109e113b581d9eb3eb8cda58))

## [0.3.1](https://github.com/mikesmitty/canbus-outpost/compare/v0.3.0...v0.3.1) (2026-10-17)


### Bug Fixes

* **lcc:** answer a repeated Unfreeze with its first result ([103fb1f](https://github.com/mikesmitty/canbus-outpost/commit/103fb1f689e4ea16d916746510c8d5586c9d0d28))
* **lcc:** release the alias before rebooting ([9db3462](https://github.com/mikesmitty/canbus-outpost/commit/9db346224dcf052ccae272054da09b880f52d485))


### Miscellaneous

* Update actions/cache action to v6 ([#7](https://github.com/mikesmitty/canbus-outpost/issues/7)) ([3e405ec](https://github.com/mikesmitty/canbus-outpost/commit/3e405ecbe0236e8702a578f87b1e819e90c3dbc9))
* Update actions/checkout action to v7 ([#8](https://github.com/mikesmitty/canbus-outpost/issues/8)) ([a9a1259](https://github.com/mikesmitty/canbus-outpost/commit/a9a1259d585852e2aeac5d6575b040a21e11128d))
* Update actions/upload-artifact action to v7 ([#9](https://github.com/mikesmitty/canbus-outpost/issues/9)) ([ea20c2c](https://github.com/mikesmitty/canbus-outpost/commit/ea20c2cfdc0d99d1b77eee26af531ba1d1d3b4ba))
* Update googleapis/release-please-action action to v5 ([#10](https://github.com/mikesmitty/canbus-outpost/issues/10)) ([fe608e2](https://github.com/mikesmitty/canbus-outpost/commit/fe608e21a85961c50adbe10631de326f299a0c18))

## [0.3.0](https://github.com/mikesmitty/canbus-outpost/compare/v0.2.1...v0.3.0) (2026-10-16)


### ⚠ BREAKING CHANGES

* **firmware:** the saved configuration format changed, so the first boot after updating starts from default settings.

### Features

* **firmware:** add LCC firmware upgrades with A/B partitions ([9fec902](https://github.com/mikesmitty/canbus-outpost/commit/9fec9022431a3bee0aae7b50b8d1eb24fd73e36b))
* **firmware:** rebuild the node on the LCC library ([a078627](https://github.com/mikesmitty/canbus-outpost/commit/a07862737fa5658a8c71d90c98996fc7dcd3b71b))
* **lcc:** add producer ranges and firmware upgrade ([cda6121](https://github.com/mikesmitty/canbus-outpost/commit/cda61210fe623f593dac092881dea92fac5042c4))
* **lcc:** add standalone OpenLCB library with host tests ([26daaf3](https://github.com/mikesmitty/canbus-outpost/commit/26daaf3669002c157862c4afe887a015c380931b))
* **lcc:** detect another node using this node's ID ([1e978a3](https://github.com/mikesmitty/canbus-outpost/commit/1e978a3b318d47d8d8b8e995b0f135cfd56620ac))
* **railcom:** report occupancy and locomotives over LCC ([0a94255](https://github.com/mikesmitty/canbus-outpost/commit/0a942559b7ec857642f05ff64a3b7018cadcdc2d))
* **servo:** add "No Change" default state option and default to closed ([8e3acc5](https://github.com/mikesmitty/canbus-outpost/commit/8e3acc5dbc9800a2b613a01acdd93002f33e7c38))


### Bug Fixes

* **ci:** check out mbedtls so picotool can hash the image ([2a094cc](https://github.com/mikesmitty/canbus-outpost/commit/2a094cced31faaf4977591a4ee146ec45f3638c5))
* **hardware:** fix railcom circuits ([3e4cef5](https://github.com/mikesmitty/canbus-outpost/commit/3e4cef5458fc2f0919f649ae8a24d963a0a8edc6))
* **lcc:** replace POSIX strnlen with portable C11 ([f7ed556](https://github.com/mikesmitty/canbus-outpost/commit/f7ed5568ebdb4551a1f5b33b9d319777cd2cd735))
* **node-id:** keep the stand-in node ID out of other vendors' ranges ([2892eb2](https://github.com/mikesmitty/canbus-outpost/commit/2892eb2bdd8351dfeab05e87e1c6822c902f0d36))


### Miscellaneous

* Configure Renovate ([#4](https://github.com/mikesmitty/canbus-outpost/issues/4)) ([2b3e3b1](https://github.com/mikesmitty/canbus-outpost/commit/2b3e3b194d279b043447a2112363e853f1ff80a5))
* **hardware:** add misc kicad files ([57013a8](https://github.com/mikesmitty/canbus-outpost/commit/57013a8b93326d6f1756d89e75506f9a02b5cea0))


### Documentation

* **claude:** describe firmware upgrades and RailCom events ([522997d](https://github.com/mikesmitty/canbus-outpost/commit/522997d699fdfa8522cafb55440c9f8ca4fc26c9))

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
