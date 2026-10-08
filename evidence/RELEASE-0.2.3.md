# PhotosVideo2x 0.2.3

User requests handled:
1. Remove circled panel text: plugin download count, current request rank and provider/downloadPriority lines are deleted, including the label and render code. No queue statistics remain.
2. Automatic local confirmation: a non-cloud-placeholder asset is treated as local without asking the user to tap. Panel shows 本地 with iCloud/check style cloud icon (icloud.and.arrow.up) once confirmed. Cloud placeholders keep the iCloud download entry and do not start a network request merely by opening the page.
3. Confirmed-local state uses the cloud upload icon so it reads as stored in iCloud/Photos local cache, previously shown as a heart-like checkmark shape.

Retained from earlier fixes: requestURLOnly:NO; panel owned by visible One Up page; display only while native top and bottom bars are visible; multiline adaptive panel; no auto-probe; preheated tiles cannot claim selection; late callbacks rejected after leaving; 2x long-press and haptics unchanged.

Actions 37782660952 success. Three macOS suites PASS (rate; file/callback/one-up/chrome policy; production lifecycle helpers). arm64/arm64e build and signing pass; genuine arm64e slice 0x80000002; RootHide layout without /var/jb. RootHide deb SHA256 196c98ba8f9b425d5e93c2b57dd0472d8aaa54aebd69decfcd2028543c5ec5a5.

Limits: not installed on the device in this step. Auto-local classification is based on the asset cloud-placeholder state, which may not prove every byte is resident; actual panel visibility, bar sync and network download still need device retest.
