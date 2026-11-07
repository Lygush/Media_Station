#!/usr/bin/env python3
# Поддельный gRPC-сервер эмулятора для tst_emulator_grpc: настоящая
# реализация gRPC (grpcio) по emulator_controller.proto из пакета эмулятора.
# Аргументы: <порт> <токен|-> <файл журнала>. Каждый sendKey пишется в журнал
# строкой «key <codeType> <eventType> <keyCode>» или «text <строка>».
import sys, os, tempfile
from concurrent import futures
import grpc
from grpc_tools import protoc

here = os.path.dirname(os.path.abspath(__file__))
gen = tempfile.mkdtemp()
protoc.main(["protoc", "-I" + here, "-I" + os.path.join(os.path.dirname(protoc.__file__), "_proto"),
             "--python_out=" + gen, "--grpc_python_out=" + gen,
             os.path.join(here, "emulator_controller.proto")])
sys.path.insert(0, gen)
import emulator_controller_pb2 as pb
import emulator_controller_pb2_grpc as rpc
from google.protobuf import empty_pb2

port, token, log = int(sys.argv[1]), sys.argv[2], sys.argv[3]

def check(ctx):
    if token == "-":
        return True
    md = dict(ctx.invocation_metadata())
    if md.get("authorization") != "Bearer " + token:
        ctx.abort(grpc.StatusCode.UNAUTHENTICATED, "bad token")
    return True

class Svc(rpc.EmulatorControllerServicer):
    def getStatus(self, request, ctx):
        check(ctx)
        return pb.EmulatorStatus(version="36.4.10.0 (fake)", uptime=12345, booted=True)
    def sendKey(self, request, ctx):
        check(ctx)
        with open(log, "a") as f:
            if request.text:
                f.write("text %s\n" % request.text)
            else:
                f.write("key %d %d %d\n" % (request.codeType, request.eventType, request.keyCode))
        return empty_pb2.Empty()

server = grpc.server(futures.ThreadPoolExecutor(max_workers=4))
rpc.add_EmulatorControllerServicer_to_server(Svc(), server)
server.add_insecure_port("127.0.0.1:%d" % port)
server.start()
print("READY", flush=True)
server.wait_for_termination()
