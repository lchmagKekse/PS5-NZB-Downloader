# PS5 NZB Downloader

A homebrew payload that turns your jailbroken PS5 into a Usenet downloader.
Give it an NZB and i will handle downloading, verifying, repairing,
and unpacking. Controlled from a small web UI running on the PS5 itself. (`http://<PS5_IP>:4202`)

The idea is you skip the PC-download-then-transfer step entirely, the console grabs it directly.

## Features

- Downloads NZBs over Usenet, TLS/SSL supported
- Verifies and repairs downloads automatically
- Unpacks archives when done
- Job queue that survives a reboot, so an interrupted download doesn't mean starting over
- Adds finished jobs to ShadowMountPlus (`manual.lst`)
- Web UI on port 4202 for adding NZBs, watching progress, and changing settings

## Requirements

- Jailbroken PS5
- Usenet provider access
- A place to grab NZBs from

## Getting started

Load the payload, open the web UI on `http://<PS5_IP>:4202`, and set your
Usenet server details in settings. From there you can upload NZBs and
watch them move through the queue.

## Credits

Built on top of:

- [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk)
- [ps5-payload-websrv](https://github.com/ps5-payload-dev/websrv)
- OpenSSL - NNTPS
- expat - NZB XML parsing
- libmicrohttpd - the web server
- rapidyenc - yEnc decoding
- libarchive - unpacking
- cJSON
- PAR2 repair math ported from [par2cmdline](https://github.com/Parchive/par2cmdline)'s reference algorithm
- Pico.css, Font Awesome, Roboto - the web UI
- ShadowMount Plus, for the mount-list integration

## Status

Personal project, still rough around the edges. Use at your own risk!

## License

GPLv3, see [`LICENSE`](LICENSE) - required by the PS5 Payload SDK, which this links against.
