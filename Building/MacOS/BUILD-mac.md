# OSCAR Build Instructions for Mac

## Prerequisites

- [Qt 5] 
- [macOS 10.12 Sierra] or higher for building (recommended: Qt 5.12)
- Xcode command line tools. (Download instructions are below.)
- Xcode - Website for the most current version for your mac distro: https://xcodereleases.com/

NOTE: Official Qt5 builds are currently made with [macOS 10.14 Catalina] and Command-Line Tools for [Xcode] 12.4.

- [Qt 6] 
- [macOS 13 Ventura] or higher for building (recommended: Qt 6.9 or higher) 
- Xcode command line tools. (Download instructions are below.)
- Xcode - Website for the most current version for your mac distro: https://xcodereleases.com/


## Setup
1. Apply all updates to your OS.  

2. (Optional) Install Xcode.  
    A. Open the Xcode's .xip file to extract it with Archive Utility, or double-click on the file itself.  
    B. Delete the .xip archive.  
    C. Move Xcode.app into /Applications.  
    D. Launch Xcode.app and agree to the license.  
    E. Uncheck "Show this window..." and close the window.  
    F. Xcode > Quit  
 
3. Install the command-line developer tools, approx. 0.6GB:  
    A. Launch Terminal.app and run:  
>		    sudo xcode-select --install  

   B. Click "Install"  
   C. Click "Agree"  

   This will download and install the latest version of the Command-Line Tools for Xcode for your version of macOS, without requiring a developer account.  

   _Alternatively, the command-line tools installer .dmg can be downloaded from the [Xcode] download site, but you will need a (free) developer account and will need to pick the appropriate download for your version of macOS.  

4. Install the Qt Opensource IDE platform  
    A. Use the Qt Online Opensource Installer: https://www.qt.io/download-qt-installer-oss.  
    
    B. Select the version you wish.  For QT5 versions, you may need to search the Archive section.  
    
    C. You do not need the following packages  
	-    Sources  
	-    iOS   
	-    Any web development package  
	
    D. Additional Libraries needed:  Qt Serial Port  

5. Install Qt   
    A. Mount and launch qt-opensource-mac dmg file.  This can be done by double-clicking on the file name.  
    B. Follow the installer's instructions. (optionally you can change the installation directory)  
        * Qt is entirely self-contained and can be installed   anywhere. It defaults to the ~/Qt/version number directory.  
        * If you only have the command-line tools installed, the Qt installer will complain that "You need to install Xcode and set up Xcode command line tools." Simply click OK.  
    C. Expand Qt select "macOS", Continue  
    D. Select "I have read and agree..." and Continue, Install  
    E. Uncheck "Launch Qt Creator", Done  
    F. Eject qt-opensource-mac installer file.  

## Build

1. Build OSCAR from the command line (All Qt versions - except where noted):  

    Launch Terminal.app  
    Type the following commands:  
> 	      mkdir YourDirectoryName  
> 	      cd YourDirectoryName  
> 	      git clone https://gitlab.com/CrimsonNape/OSCAR-SQL.git OSCAR-code  
> 	      mkdir build  
> 	      cd build  
> 	      Qt5:  ~/Qt/Qt version number/clang_64/bin/qmake ../OSCAR-code/oscar/oscar.pro  
> 	      Qt6:  ~/Qt/Qt version number/macos/bin/qmake ../OSCAR-code/oscar/oscar.pro  
> 	      make  

2. (Optional) Package for distribution:

>        make dist-mac  

   The dmg is at OSCAR.dmg in the build directory.  

## (Optional) Using Qt Creator

1. Launch Qt Creator where you installed Qt above, by default ~/Qt5.12.8/Qt Creator.app.
2. File > Open File or Project... and select ~/OSCAR-code/oscar/oscar.pro (or wherever you cloned it above), then click "Configure Project".
3. Configure building:
    1. Click on "Projects" in the left panel.
    2. Under **Build Settings**, in the "Edit build configuration" drop-down menu, select "Release".
    3. Click to expand "Details" for the **qmake** build step.
    4. Uncheck "Enable Qt Quick Compiler", click "No" to defer recompiling.
4. Configure packaging for distribution:
    1. Click "Clone..." to the right of the "Edit build configuration" drop-down menu.
    2. Name the new configuration "Deploy".
    3. Click to expand "Details" for the **Make** build step.
    4. Set the Make arguments for the Make step to "dist-mac".
5. To build OSCAR, select "Release" from the "oscar" button in the left panel. Then select Build > Build Project "oscar". The application is in OSCAR.app.
6. To build OSCAR and package for distribution, select "Deploy" from the "oscar" button in the left panel. Then select Build > Build Project "oscar". The dmg is at OSCAR.dmg.
    * Progress in "Compile Output" will pause for several seconds while "Creating .dmg". This is normal.

## Apple Silicon Native Build (New)

For users with Apple Silicon Macs (M1 or later) who want native ARM64 performance:

### Prerequisites
- Qt 5.15.9+ with ARM64 support: `brew install qt@5`
- Apple Silicon Mac (M1/M2/M3)

### Quick Build
```bash
# Simple ARM64 build (recommended for Apple Silicon users)
./Building/MacOS/build-arm64.sh
```

### Performance Benefits
- **2-3x faster** than Rosetta 2 translation
- **Lower memory usage** 
- **Native performance** on Apple Silicon hardware

### Manual Build Steps
If you prefer manual control:
```bash
mkdir build-arm64 && cd build-arm64
/opt/homebrew/opt/qt@5/bin/qmake ../oscar/oscar.pro -spec macx-clang
make -j$(sysctl -n hw.logicalcpu)
```

The resulting `OSCAR.app` will be natively compiled for Apple Silicon.

---

## Build Compatibility

- **Intel Mac**: Use original Qt 5.12.8 instructions above
- **Apple Silicon Mac**: Use either approach:
  - Original instructions (runs via Rosetta 2)
  - New ARM64 build (native performance) with newer Qt 5.15.9+

[Qt 5.12.8]: http://download.qt.io/archive/qt/5.12/5.12.8/qt-opensource-mac-x64-5.12.8.dmg
[macOS 10.14 Mojave]: https://apps.apple.com/us/app/macos-mojave/id1398502828?ls=1&mt=12
[macOS 10.13 High Sierra]: https://apps.apple.com/us/app/macos-high-sierra/id1246284741?ls=1&mt=12
[macOS 10.12 Sierra]: https://apps.apple.com/us/app/macos-sierra/id1127487414?ls=1&mt=12
[Xcode]: https://developer.apple.com/download/more/
