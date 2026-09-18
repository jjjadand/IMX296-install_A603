# Host-side build for the A603 IMX296LQ package.
.PHONY: overlay tools driver deb all

overlay:
	./build-overlay.sh

tools:
	$(MAKE) -C tools

driver:
	./build-driver.sh

deb: overlay tools
	./build-deb.sh

all: deb
