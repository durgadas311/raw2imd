# raw2imd
## Convert raw/flat diskette images to IMD

### Usage

Invoking 'raw2imd' with no arguments will print help information.

#### Example: Kaypro DD/DS (400K)

Using a "plain" raw disk image, and creating the Kaypro physical sector skew,
the command would be:
```
raw2imd -c 40 -h 2 -s 10 -l 512 -m -o 0 -O 10 -p 2 -k -4 -T "Some Title" mydisk.raw mydisk.imd
```

Using a Kaypro simulator "logdisk" image, the same command would be:
```
raw2imd -L -k -4 -T "Some Title" mydisk.logdisk mydisk.imd
```

### Features

'raw2imd' supports basic "flat" sector images. Such files
contains the sectors of the diskette in order, for example:
```
Track 0 Side 0 Sector 1...N
Track 0 Side 1 Sector 1...N
Track 1 Side 0 Sector 1...N
...
```
Assuming sectors are numbered starting with "1".

'raw2imd' also supports the special format known as "logdisk",
which is used for several simulations from http://sims.durgadas.com.
These files are similar to the raw/flat image file, but have a 128-byte
"header" appended which contains an ASCII string/line that describes
the diskette geometry. This eliminates the need to specify most
paramters on the 'raw2imd' commandline.

### Building

This repo uses another repo, from http://offog.org/git/dumpfloppy.git.
Default cloning of this repo will not expand the submodule.
Either clone with:
```
git clone --recurse-submodules <this>
```
or run these commands after the default clone:
```
git submodule init
git submodule update
```

Once the submodule is expanded, the command "make" will
build both 'raw2imd' and a local copy of 'imdcat'.

