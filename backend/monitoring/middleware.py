from django.conf import settings
from django.core.exceptions import PermissionDenied
from django.shortcuts import redirect
from django.urls import Resolver404, resolve

from .permissions import is_viewer

# Únicas páginas de la plataforma que puede abrir un Visualizador. Los comandos
# se filtran además por variable en la vista (ver VIEWER_CONTROL_VARIABLES).
VIEWER_ALLOWED_URLS = {
    "monitoring:dashboard",
    "monitoring:api-latest",
    "monitoring:dashboard-order",
    "monitoring:sensor-command",
    "login",
    "logout",
}


class ViewerReadOnlyMiddleware:
    """Limita al rol Visualizador al dashboard: cualquier otra página lo
    devuelve al dashboard, y los POST fuera de las URLs permitidas se rechazan."""

    def __init__(self, get_response):
        self.get_response = get_response

    def __call__(self, request):
        if (
            request.user.is_authenticated
            and not request.path_info.startswith(settings.MEDIA_URL)
            and is_viewer(request.user)
        ):
            try:
                match = resolve(request.path_info)
                name = f"{match.namespace}:{match.url_name}" if match.namespace else match.url_name
            except Resolver404:
                name = None
            if name not in VIEWER_ALLOWED_URLS:
                if request.method != "GET":
                    raise PermissionDenied
                return redirect("monitoring:dashboard")
        return self.get_response(request)
