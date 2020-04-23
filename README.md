Rauc Disk Updater
================

`rauc-disk-updater` interacts with udev and Rauc to automatically install
bundles from plugged in USB devices and SD-cards.

Features
--------

* Script Hook Support
* Controllable via D-Bus interface
* Automatic Mounting
* Validation of found bundles
* Automatic bundle installation without user interaction
* USB devices (scsi) and SD-card Support (mmc)
* Support for multiple devices

How Does It Work
----------------

1. Create a Bundle with matching `compatible` and `version` string. 
2. Copy bundle on USB stick with a filesystem supported by the target machine.
3. Plug in USB stick in the target machine.
4. `rauc-disk-updater` search for bundles on the stick. Each bundle is validated
   by rauc. 
5. Either a hook script determines the installation or the D-Bus method
   `install` is called in order to start the installation of a found bundle.


Compiling & Installation
------------------------

```bash
mkdir build
cd build
cmake .. 
make
make install
systemctl enable --now rauc-disk-updater.service
```


Usage
-----

```bash
/usr/bin/rauc-disk-updater --help
Usage:
  rauc-usb-updater [OPTION?] 

Help Options:
  -h, --help            Show help options

Application Options:
  -s, --script-file     Script file
  -v, --version         Version information
```


Script API
----------

The script is called after all mounted partitons of an attached device are
searched through and minimum one bundle is found. The number of bundles is
passed as variable `BUNDLES`. For each bundle with index `X`, the variables
`BUNDLE_VERSION_X` and `BUNDLE_PATH_X` are passed. In order to trigger the
installation of a bundle, the exit code of the script is to the index `X`. If
the exist code is set to `0`, the installation will be aborted.

Following script automatically installs the bundle with highest version.

```bash
#!/bin/bash
VERSION=""
INDEX=0

for i in $(seq 1 $BUNDLES); do
    BUNDLE_VERSION="BUNDLE_VERSION_${i}"
    if [ "${!BUNDLE_VERSION}" '>' "$VERSION" ]; then
	VERSION="${!BUNDLE_VERSION}"
	INDEX="$i"
    fi
done

exit $INDEX
esac
```


Contributing
------------

Fork the repository and send us a pull request.
