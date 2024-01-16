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
    #api = Api.objects.get()
    context = { }
    context = { 'api_list' : Api.objects.filter() }

    return render(request, 'rocpd/api_summary.html', context)

def opSummary(request):
    return render(request, 'rocpd/op_summary.html', None)

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
    return HttpResponse("autograd index")

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
