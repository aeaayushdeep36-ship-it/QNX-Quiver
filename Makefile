QCC ?= qcc
QCC_TARGET ?= gcc_ntox86_64
SECPOLCOMPILE ?= secpolcompile
CFLAGS ?= -std=c11 -D_QNX_SOURCE -Wall -Wextra -Werror -O2
INCLUDES = -Iinclude

.PHONY: all protocol-check pci-service pci-rm pci-client client-probe can-ddk can-probe policy-check clean
all: protocol-check pci-service pci-rm pci-client client-probe can-ddk can-probe policy-check

protocol-check: build/protocol-check
build/protocol-check: qnx/protocol_check.c include/quiver_protocol.h
	$(QCC) -V$(QCC_TARGET) $(CFLAGS) $(INCLUDES) -o $@ qnx/protocol_check.c

pci-service: build/quiver-pci-service
build/quiver-pci-service: qnx/dev-quiver/pci_service.c include/quiver_protocol.h
	$(QCC) -V$(QCC_TARGET) $(CFLAGS) $(INCLUDES) -o $@ qnx/dev-quiver/pci_service.c -lpci

pci-rm: build/quiver-pci-rm
build/quiver-pci-rm: qnx/dev-quiver/pci_rm.c include/quiver_protocol.h
	$(QCC) -V$(QCC_TARGET) $(CFLAGS) $(INCLUDES) -o $@ qnx/dev-quiver/pci_rm.c -lpci -lsecpol

pci-client: build/quiver-client.o
build/quiver-client.o: qnx/dev-quiver/client.c include/quiver_client.h include/quiver_protocol.h
	$(QCC) -V$(QCC_TARGET) $(CFLAGS) $(INCLUDES) -c -o $@ qnx/dev-quiver/client.c

client-probe: build/quiver-client-probe
build/quiver-client-probe: qnx/dev-quiver/client_probe.c build/quiver-client.o include/quiver_client.h include/quiver_protocol.h
	$(QCC) -V$(QCC_TARGET) $(CFLAGS) $(INCLUDES) -o $@ qnx/dev-quiver/client_probe.c build/quiver-client.o

can-ddk: build/dev-can-quiver
build/dev-can-quiver: qnx/dev-can-quiver/quiver_can.c build/quiver-client.o include/quiver_client.h include/quiver_protocol.h
	$(QCC) -V$(QCC_TARGET) $(CFLAGS) $(INCLUDES) -o $@ qnx/dev-can-quiver/quiver_can.c build/quiver-client.o -lcan -lsecpol -lslog2

can-probe: build/quiver-can-probe
build/quiver-can-probe: qnx/dev-can-quiver/can_probe.c build/quiver-client.o include/quiver_client.h include/quiver_protocol.h
	$(QCC) -V$(QCC_TARGET) $(CFLAGS) $(INCLUDES) -o $@ qnx/dev-can-quiver/can_probe.c build/quiver-client.o

policy-check: build/qnx_quiver_policy_fragment.bin
build/qnx_quiver_policy_fragment.bin: qnx/security/qnx_quiver.policy
	$(SECPOLCOMPILE) -o $@ $<

clean:
	$(RM) build/protocol-check build/quiver-pci-service build/quiver-pci-rm build/quiver-client.o build/quiver-client-probe build/dev-can-quiver build/quiver-can-probe build/qnx_quiver_policy_fragment.bin
