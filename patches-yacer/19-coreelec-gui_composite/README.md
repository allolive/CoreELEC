# GUI compositing: present the frames it draws

Kodi skips the GUI render pass during fullscreen video when no control
dirtied itself. With HDR GUI compositing on, such a frame still writes the
back buffer, so it still has to be presented. These patches make the AML
backend do that.
