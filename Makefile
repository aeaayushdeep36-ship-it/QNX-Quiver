QCC ?= qcc
QCC_TARGET ?= gcc_ntox86_64
SECPOLCOMPILE ?= secpolcompile
CFLAGS ?= -std=c11 -D_QNX_SOURCE -Wall -Wextra -Werror -O2
INCLUDES = -Iinclude

.PHONY: all protocol-check pci-service pci-rm pci-client client-probe denied-client-probe can-ddk can-probe can-saturation-probe peripheral-services supervisor policy-check host-test clean
all: protocol-check pci-service pci-rm pci-client client-probe denied-client-probe can-ddk can-probe can-saturation-probe peripheral-services supervisor policy-check

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

denied-client-probe: build/quiver-client-denied-probe
build/quiver-client-denied-probe: qnx/dev-quiver/denied_client_probe.c
	$(QCC) -V$(QCC_TARGET) $(CFLAGS) $(INCLUDES) -o $@ qnx/dev-quiver/denied_client_probe.c

can-ddk: build/dev-can-quiver
build/dev-can-quiver: qnx/dev-can-quiver/quiver_can.c build/quiver-client.o include/quiver_client.h include/quiver_protocol.h
	$(QCC) -V$(QCC_TARGET) $(CFLAGS) $(INCLUDES) -o $@ qnx/dev-can-quiver/quiver_can.c build/quiver-client.o -lcan -lsecpol -lslog2

can-probe: build/quiver-can-probe
build/quiver-can-probe: qnx/dev-can-quiver/can_probe.c build/quiver-client.o include/quiver_client.h include/quiver_protocol.h
	$(QCC) -V$(QCC_TARGET) $(CFLAGS) $(INCLUDES) -o $@ qnx/dev-can-quiver/can_probe.c build/quiver-client.o

can-saturation-probe: build/quiver-can-saturation-probe
build/quiver-can-saturation-probe: qnx/dev-can-quiver/can_saturation_probe.c include/quiver_protocol.h
	$(QCC) -V$(QCC_TARGET) $(CFLAGS) $(INCLUDES) -o $@ qnx/dev-can-quiver/can_saturation_probe.c

peripheral-services: build/quiver-imu build/quiver-actuator build/quiver-trace build/quiver-controller
build/quiver-imu: qnx/services/imu_service.c qnx/services/quiver_services.h include/quiver_client.h include/quiver_protocol.h build/quiver-client.o
	$(QCC) -V$(QCC_TARGET) $(CFLAGS) $(INCLUDES) -Iqnx/services -o $@ qnx/services/imu_service.c build/quiver-client.o -lm
build/quiver-actuator: qnx/services/actuator_client.c qnx/services/quiver_services.h include/quiver_client.h include/quiver_protocol.h build/quiver-client.o
	$(QCC) -V$(QCC_TARGET) $(CFLAGS) $(INCLUDES) -Iqnx/services -o $@ qnx/services/actuator_client.c build/quiver-client.o -lm
build/quiver-trace: qnx/services/trace_capture.c qnx/services/quiver_services.h include/quiver_client.h include/quiver_protocol.h build/quiver-client.o
	$(QCC) -V$(QCC_TARGET) $(CFLAGS) $(INCLUDES) -Iqnx/services -o $@ qnx/services/trace_capture.c build/quiver-client.o -lm
build/quiver-controller: qnx/services/controller_demo.c qnx/services/quiver_services.h include/quiver_client.h include/quiver_protocol.h build/quiver-client.o
	$(QCC) -V$(QCC_TARGET) $(CFLAGS) $(INCLUDES) -Iqnx/services -o $@ qnx/services/controller_demo.c build/quiver-client.o -lm

supervisor: build/quiver-supervisor
build/quiver-supervisor: qnx/supervisor/quiver_supervisor.c
	$(QCC) -V$(QCC_TARGET) $(CFLAGS) $(INCLUDES) -o $@ qnx/supervisor/quiver_supervisor.c -lsecpol

policy-check: build/qnx_quiver_policy_fragment.bin
build/qnx_quiver_policy_fragment.bin: qnx/security/qnx_quiver.policy
	$(SECPOLCOMPILE) -o $@ $<

host-test:
	python3 -m unittest discover -s tests -v
	python3 host/sim_engine/quiver_sim.py --self-test

clean:
	$(RM) build/protocol-check build/quiver-pci-service build/quiver-pci-rm build/quiver-client.o build/quiver-client-probe build/quiver-client-denied-probe build/dev-can-quiver build/quiver-can-probe build/quiver-can-saturation-probe build/quiver-imu build/quiver-actuator build/quiver-trace build/quiver-controller build/quiver-supervisor build/qnx_quiver_policy_fragment.bin
