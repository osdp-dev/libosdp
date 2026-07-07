---
release_date: 2023-10-28
release_version: v2.4.0
---

This release marks the end of 2.x version of libosdp. Bring some minor
updates and fixes.

## Enhancements

- libosdp: Add new API to set a log callback function
- unit-test: Add line noise simulation methods
- pytest: Add new test for CP/PD status reports
- pyosdp: pd: Expose a new API to check online status
- libosdp: Update to new logger API
- Expose new configure time macro REPO_ROOT
- configure: Add new option --debug to enable debug symbols
- make: Don't remove and rebuild unit-test target each time
- cmake: Add hook to checkout submodules
- pd: Use teardown in peripheral destructor only if context exists
- cp: Use teardown in control panel destructor only if context exists

## Fixes

- libosdp: Fix stable API change violation
- unit-tests: Fix warning due to change of fclose sinature
- libosdp: cmake: Fix static builds with local openssl
- c++: add header guards
- pytest: testlib: Fix bug in retry count
- pytest: Make CP and PD restart-able
- API: common: Fix osdp_get_status_mask for PD mode
- Fix null pointer deref issue osdp_reply_name
- doc: Update domain name to sidcha.dev as gotomain.io has expired
- Update copyright year to 2023
- Remove obsolete travis CI config file and it's refs
- check osdp close file status (according prototype function comments)
- save PD Local Status
