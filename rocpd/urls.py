from django.urls import path

from . import views

app_name = 'rocpd'

urlpatterns = [
    path('', views.dashboard, name='dashboard'),
    path('api', views.apiSummary, name='api_index'),
    path('op', views.opSummary, name='op_index'),
    path('kernel', views.kernelSummary, name='kernel_index'),
    path('trace', views.traceSummary, name='trace_index'),
    path('tracedata', views.traceData, name='trace_data'),
    path('graph', views.graphSummary, name='graph_index'),
    path('graph_kernel/<str:pk>', views.graphKernel, name='graph_kernel'),
    path('graph_launch/<str:pk>', views.graphLaunch, name='graph_launch'),
    path('autograd', views.autogradSummary, name='autograd_index'),
    path('autocop', views.autocop, name='autocop'),
    path('metadata', views.MetadataView.as_view(), name='metadata'),
    path('doc', views.documentation, name='doc_index'),
]
