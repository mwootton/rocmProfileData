from django.shortcuts import render
from django.http import HttpResponse
from django.views import generic

from .models import Api
from .models import Op
from .models import Metadata
from django.db import models

# Create your views here.

def index(request):
    return HttpResponse("rocpd index")

def dashboard(request):
    return render(request, 'rocpd/dashboard.html', None)

def apiSummary(request):
    #context = { 'api_list' : Api.objects.filter() }

    # FIXME: use the ORM model

    # Do a summary for now
    apis = Api.objects.raw("select row_number() over () as id, apiName as sname, count(*) as total_calls from api group by apiName")
    context = { 'api_list' : apis }

    return render(request, 'rocpd/api_summary.html', context)

def opSummary(request):
    ops = Op.objects.raw("select row_number() over () as id, opType as sopType, count(*) as total_execs from op group by opType")
    context = { 'op_list' : ops } 
    return render(request, 'rocpd/op_summary.html', context)

def kernelSummary(request):
    kernels = Api.objects.raw("select row_number() over () as id, * from top")
    context = { 'kernel_list' : kernels }
    return render(request, 'rocpd/kernel_summary.html', context) 

def traceSummary(request):
    # FIXME: load total time range, apicalls to filer, gpus to filter, etc
    context = { 'first' : 100,
                'last' : 1000,
              }
    return render(request, 'rocpd/trace_controls.html', context)

def traceData(request):
    from django.conf import settings
    rpdfile = getattr(settings, "DATABASES", None)['default']['NAME']

    from rocpd.util.chrometracing import generateJson
    import io
    mem = io.StringIO()

    import argparse
    args = argparse.Namespace()
    args.input_rpd = rpdfile
    args.format = "object"
    # FIXME read args from url (start end)
    args.start = "0%"
    args.end = "100%"

    generateJson(mem, args)

    response = HttpResponse(
        content_type='text/json',
    )
    response['Content-Disposition'] = f'attachment; filename="trace.json"'
    response.write(f"{mem.getvalue()}")
    return response   

    #return HttpResponse(f"{mem.getvalue()}")

def graphSummary(request):
    graphs = Api.objects.raw("select row_number() over () as id, *, (end-start)/1000 as duration_us from ext_graph")
    context = { 'graph_list' : graphs }
    return render(request, 'rocpd/graph_summary.html', context)

def graphLaunch(request, pk):
    graphs = Op.objects.raw('select row_number() over () as id, * from graphLaunch where graphExec = %s', (pk, ))
    context = { 'graph_list' : graphs,
                'graphExec' : pk,
              }
    return render(request, 'rocpd/graph_launch.html', context)

def graphKernel(request, pk):
    kernels = Api.objects.raw('select row_number() over () as id, * from graphKernel where graphExec = %s', (pk, ))
    context = { 'kernel_list' : kernels,
                'graphExec' : pk,
              }
    return render(request, 'rocpd/graph_kernel.html', context)

def autogradSummary(request):
    from django.conf import settings
    rpdfile = getattr(settings, "DATABASES", None)['default']['NAME']

    from rocpd.util.autogradKernelList import generateKernelList
    import io
    mem = io.StringIO()

    import argparse
    args = argparse.Namespace()
    args.input_rpd = rpdfile

    generateKernelList(mem, args)

    return HttpResponse(f"{mem.getvalue()}")

def autocop(request):
    return HttpResponse("index")

def metadata(request):
    return HttpResponse("metadata")

def documentation(request):
    try:
        import markdown
    except:
        context = { 'html' : 'Please "pip install markdown" to see formatted documentation' }
        return render(request, 'rocpd/documentation.html', context)

    import os
    path = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(path, 'static/rocpd/README.md')
    readme = open(path)
    md = readme.read()
    context = { 'html' : markdown.markdown(md) }
    return render(request, 'rocpd/documentation.html', context)

class MetadataView(generic.ListView):
    model = Metadata
    template_name = 'rocpd/metadata_list.html'
