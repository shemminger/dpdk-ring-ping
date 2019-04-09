# Ring-ping

Ring ping is a DPDK application to measure CPU latency.

## Instalation

Requires DPDK from https://dpdk.org

Build:
```bash
export RTE_SDK={path to DPDK}
export RTE_TARGET=build
make
```

## Usage
Since this does not use any actual hardware, it can be run as non-root.
The DPDK arguments tell it to use 2 cores and not use huge pages or PCI.
To run the test for 30 seconds

```bash
./build/app/rping -c 3 -n 2 --no-pci --no-huge -- -t 30
```

## Contributing
Pull requests are welcome.

## License
BSD 3-clause (same as DPDK)


