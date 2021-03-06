# AirDC++ Web Client

AirDC++ Web Client is a cross-platform file sharing client for Advanced Direct Connect network. It has a web-based user interface that can be accessed with a web browser.

## Installation

### Installing packages on Ubuntu

Ubuntu 14.04 or newer is required for installing the client.

#### Install tools

`sudo apt-get install gcc g++ git cmake npm nodejs`

#### Install libraries

`sudo apt-get install libbz2-dev zlib1g-dev libssl-dev libstdc++6 libminiupnpc-dev libnatpmp-dev libtbb-dev libgeoip-dev libboost1.5*-dev libboost-regex1.5* libboost-thread1.5* libboost-system1.5* libleveldb-dev`

#### Install WebSocket++

If you are running Ubuntu 15.10 or newer, you may use the following command to install the package:

`sudo apt-get install libwebsocketpp-dev`

If you are running an older version of Ubuntu, run the following commands to install the package manually:

```
git clone git://github.com/zaphoyd/websocketpp.git
mkdir websocketpp/build && cd websocketpp/build
cmake ..
make
sudo make install
cd..
cd..
```

### Download the client

```
git clone https://github.com/maksis/airdcpp-webclient.git
cd airdcpp-webclient
```

### Compile and install

```
cmake .
make -j4
sudo make install
```
This will compile the client with four simultaneous threads.

### Configure and run

When starting the client for the first time, you need to run the initial configuration script. This will set up the server ports and administrative user account for accessing web user interface.

```
airdcppd --configure
```

You may now start the client normally

```
airdcppd
```

Access the user interface with your web browser and log in with the user account that was created. If you accepted the default ports and the client is running on the same computer, the following address can be used:

[http://localhost:5600](http://localhost:5600)


## Updating

Fetch the latest files

```
git pull
```

Remove the old installation. Note that you won't be able to access the Web UI after this command. If you want to keep using the client while the new version is being compiled, You may also choose to perform this step just before running the 'make install' command in the installation section. 

```
sudo make uninstall
```

Follow the instructions in the [Compile and install](#compile-and-install) section to install the new version.


## Uninstalling

```
make uninstall
```

You may also remove the source and settings directories as well if you are not going to need them later.
