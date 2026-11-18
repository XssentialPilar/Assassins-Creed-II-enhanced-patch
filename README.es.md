# Assassin’s Creed II Enhanced Patch

[English](README.md) · [Descargar la última versión](../../releases/latest)

Parche comunitario para la edición de Steam de Assassin’s Creed II en Windows.

## Funciones

- Partida a pantalla completa en la relación de aspecto elegida, con escala proporcional para el HUD y los menús.
- Avisos de acción, dinero, arma y minimapa anclados a los bordes. El mapa llena el radar y los velos de la tienda y del selector de armas cubren la pantalla.
- Reproducción proporcional de la introducción y los vídeos pregrabados, con fondos que rellenan la pantalla.
- Se elimina la espera interna de 60 FPS al seleccionar una frecuencia superior a 60 Hz.
- Integración con la entrada del juego. Identifica los mandos DualShock 4 y Xbox compatibles, muestra los iconos de su familia y transmite las órdenes de vibración de ambos motores.

## Instalación

Requiere la compilación **1843459** de la aplicación **33230** de Steam en Windows. Abre las propiedades del juego en Steam y selecciona la opción para explorar sus archivos instalados.

1. Cierra el juego. Si ya hay un `d3d9.dll` o `SDL3.dll`, guarda esas copias fuera de la carpeta del juego.
2. Copia `d3d9.dll` y `SDL3.dll` del ZIP junto a `AssassinsCreedIIGame.exe`.
3. Inicia el juego desde Steam. Elige la resolución y la frecuencia en sus opciones.

Para desinstalarlo, cierra el juego, retira las dos DLL de esta versión y recupera las copias que guardaste. Si tienes otro mod que usa `d3d9.dll`, elige qué DLL cargar. El parche se carga junto al juego y conserva el ejecutable, las partidas y los ajustes.

## Pruebas

Se comprobó en partida a **1920×1200** y **1440×1080**. Las pruebas de geometría cubren además 16:9, 3:2, 5:4, 4:5, 2:1 (18:9), 21:9, 2.375:1, 1.85:1, 2.35:1, 2.39:1, 2.40:1, 2.76:1, 9:16, 19.5:1 y 20:9.

Con mandos virtuales se comprobó el cambio de familia, los iconos, la reconexión y la vibración sostenida e independiente. Sigue pendiente la prueba con mandos físicos por USB y Bluetooth. Para mostrar los iconos de la familia correcta, Windows debe exponer la identidad del mando. El software que traduce la entrada puede presentar otro dispositivo.

Los diagnósticos se guardan localmente en `%TEMP%\ac2-display-trace-*.csv`. Consulta las [instrucciones de compilación](BUILD.md) y los [avisos de terceros](THIRD_PARTY.md).

El código propio usa la licencia [MIT](LICENSE). Assassin’s Creed II es un juego de Ubisoft. Este proyecto es independiente.
