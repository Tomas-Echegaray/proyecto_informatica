/*
 * ServidorZombie.c  -  Version 4 (protocolo de invitacion)
 *
 * Compilar:  gcc ServidorZombie.c -o servidor $(mysql_config --cflags --libs) -lpthread
 * Ejecutar:  ./servidor
 *
 * INDICE (donde esta cada cosa)
 *  1. Constantes y estructuras      4. Conectados:  PonConectado, DamePosicion, EliminaConectado,
 *  2. Variables globales                            DameConectados, NotificaConectados
 *  3. Mandar mensajes:              5. Partidas:    BuscaPartidaDeJugador, DamePosicionEnPartida, CreaPartida
 *     Enviar, EnviarAPartida        6. Juego:       Distancia, ContarVivos, Ocupada, IniciaPartida, EnviarEstado,
 *  7. Peticiones:                                   MueveZombie, TerminaPartida, AvanzaTurno, SaleJugador
 *     ContarFilas, Escapa, SeparaUsuarioContra, AtiendeRegistro, AtiendeLogin, AtiendeConsulta,
 *     AtiendeInvitacion, ComprobarRespuestas, AtiendeRespuesta, AtiendeInicio, AtiendeMovimiento, AtiendeChat, AtiendeMensaje
 *  8. AtenderCliente (el thread de cada cliente)      9. main
 *
 * El protocolo completo esta en Protocolo.txt (cada mensaje es "codigo/datos" y acaba en '\n')
 */

#define _GNU_SOURCE		//Necesario para strtok_r si se compila con -std=c99
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <mysql.h>

/* ---------------- 1. CONSTANTES Y ESTRUCTURAS ---------------- */

#define PUERTO 9080			//Si lo cambiais aqui, cambiadlo tambien en el cliente
#define COLUMNAS 30			//Tablero (tiene que ser igual que en el cliente)
#define FILAS 34
#define MAX_JUG 6			//Maximo de jugadores por partida
#define MAX_PARTIDAS 20
#define RONDAS_PARA_GANAR 20	//Rondas que hay que sobrevivir para ganar (provisional hasta que haya pistola)

//Estados de una partida
#define LIBRE 0			//Hueco sin usar
#define INVITANDO 1		//Hemos mandado las invitaciones y faltan respuestas
#define LISTA 2			//Todos han aceptado, falta que el anfitrion la inicie
#define JUGANDO 3		//Se esta jugando

typedef struct {
	//Estructura de cliente conectado
	char nombre[20];
	int socket;
} Conectado;

typedef struct {
	//Lista de clientes conectados
	Conectado conectados[100];
	int num;
} ListaConectados;

typedef struct {
	//Una partida. El jugador 0 es siempre el anfitrion (el que invita)
	int estado;					//LIBRE, INVITANDO, LISTA o JUGANDO
	int num;					//Numero de jugadores
	char nombre[MAX_JUG][20];
	int socket[MAX_JUG];		//-1 si se ha desconectado
	int respuesta[MAX_JUG];		//Respuesta a la invitacion: 0 pendiente, 1 acepta, 2 rechaza
	int vivo[MAX_JUG];
	int x[MAX_JUG];
	int y[MAX_JUG];
	int zx, zy;					//Posicion del zombie
	int turno;					//Posicion del jugador al que le toca
	int ronda;
} Partida;

/* ---------------- 2. VARIABLES GLOBALES ---------------- */

//Estructura necesaria para acceso excluyente. Un solo mutex protege TODO (conectados, partidas y base de datos)
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

MYSQL *conn;						//Conexion con la base de datos (global para no pasarla a todas las funciones)
ListaConectados miListaConectados;	//Al ser global empieza con num = 0
Partida partidas[MAX_PARTIDAS];		//El id de una partida es su posicion en este vector

/* ---------------- 3. MANDAR MENSAJES ---------------- */

void Enviar(int sock, char *formato, ...)
{
	//Funciona como un printf pero escribe en el socket. Le ponemos un '\n' al final para que el cliente separe los mensajes
	char mensaje[2048];
	va_list args;
	va_start(args, formato);
	vsnprintf(mensaje, sizeof(mensaje) - 2, formato, args);
	va_end(args);
	strcat(mensaje, "\n");
	send(sock, mensaje, strlen(mensaje), MSG_NOSIGNAL);	//MSG_NOSIGNAL: si el cliente se ha ido no se cae el servidor
}

void EnviarAPartida(Partida *p, char *formato, ...)
{
	//Igual que Enviar pero se lo manda a todos los jugadores de la partida (menos a los desconectados)
	char mensaje[2048];
	va_list args;
	va_start(args, formato);
	vsnprintf(mensaje, sizeof(mensaje), formato, args);
	va_end(args);
	int k;
	for (k = 0; k < p->num; k++)
	{
		if (p->socket[k] >= 0)
		{ Enviar(p->socket[k], "%s", mensaje); }
	}
}

/* ---------------- 4. LISTA DE CONECTADOS ---------------- */

int PonConectado(ListaConectados *lista, char nombre[20], int socket)
{
	//Anade un nuevo conectado. 0 chill, -1 si llena y no pudo anadir
	if (lista->num == 100)
	{ return -1; }
	strcpy(lista->conectados[lista->num].nombre, nombre);
	lista->conectados[lista->num].socket = socket;
	lista->num++;
	return 0;
}

int DamePosicion(ListaConectados *lista, char nombre[20])
{
	//Devuelve la posicion en la lista (0 en adelante) o -1 si no lo encuentra
	int i;
	for (i = 0; i < lista->num; i++)
	{
		if (strcmp(lista->conectados[i].nombre, nombre) == 0)
		{ return i; }
	}
	return -1;
}

int EliminaConectado(ListaConectados *lista, char nombre[20])
{
	//0 si elimina, -1 si no existe
	int posicion = DamePosicion(lista, nombre);
	if (posicion == -1)
	{ return -1; }
	int i;
	for (i = posicion; i < lista->num - 1; i++)
	{ lista->conectados[i] = lista->conectados[i + 1]; }	//Movemos todos una posicion hacia atras
	lista->num--;
	return 0;
}

void DameConectados(ListaConectados *lista, char conectados[2300])
{
	//Rellena "n/nombre1/nombre2/..." (primero va el numero de conectados)
	sprintf(conectados, "%d", lista->num);
	int i;
	for (i = 0; i < lista->num; i++)
	{
		strcat(conectados, "/");
		strcat(conectados, lista->conectados[i].nombre);
	}
}

void NotificaConectados()
{
	//Manda la lista de conectados a TODOS los conectados. Se llama cada vez que la lista cambia (login o desconexion)
	char lista[2300];
	DameConectados(&miListaConectados, lista);
	int j;
	for (j = 0; j < miListaConectados.num; j++)
	{ Enviar(miListaConectados.conectados[j].socket, "6/%s", lista); }
}

/* ---------------- 5. PARTIDAS ---------------- */

int BuscaPartidaDeJugador(char *nombre)
{
	//Devuelve el id de la partida en la que esta el jugador (invitado o jugando), o -1 si no esta en ninguna
	int i, k;
	for (i = 0; i < MAX_PARTIDAS; i++)
	{
		if (partidas[i].estado == LIBRE)
		{ continue; }
		for (k = 0; k < partidas[i].num; k++)
		{
			if (strcmp(partidas[i].nombre[k], nombre) == 0)
			{ return i; }
		}
	}
	return -1;
}

int DamePosicionEnPartida(Partida *p, char *nombre)
{
	//Devuelve la posicion del jugador dentro de la partida (0 = anfitrion) o -1 si no esta
	int k;
	for (k = 0; k < p->num; k++)
	{
		if (strcmp(p->nombre[k], nombre) == 0)
		{ return k; }
	}
	return -1;
}

int CreaPartida()
{
	//Busca un hueco libre, lo deja limpio y devuelve su id (o -1 si no queda ninguno)
	int i;
	for (i = 0; i < MAX_PARTIDAS; i++)
	{
		if (partidas[i].estado == LIBRE)
		{
			memset(&partidas[i], 0, sizeof(Partida));
			partidas[i].estado = INVITANDO;
			return i;
		}
	}
	return -1;
}

/* ---------------- 6. JUEGO ---------------- */

int Distancia(int x1, int y1, int x2, int y2)
{
	//Cuantos pasos (arriba/abajo/izquierda/derecha, sin diagonales) hay entre dos casillas
	return abs(x1 - x2) + abs(y1 - y2);
}

int ContarVivos(Partida *p)
{
	int k, vivos = 0;
	for (k = 0; k < p->num; k++)
	{ vivos = vivos + p->vivo[k]; }		//vivo vale 1 o 0
	return vivos;
}

int Ocupada(Partida *p, int x, int y)
{
	//1 si en esa casilla hay el zombie o un jugador vivo
	int k;
	if (p->zx == x && p->zy == y)
	{ return 1; }
	for (k = 0; k < p->num; k++)
	{
		if (p->vivo[k] && p->x[k] == x && p->y[k] == y)
		{ return 1; }
	}
	return 0;
}

void IniciaPartida(Partida *p)
{
	//Ponemos al zombie y a los jugadores en casillas aleatorias (los jugadores, lejos del zombie)
	int k, x, y;
	for (k = 0; k < p->num; k++)
	{ p->vivo[k] = 0; }		//Asi Ocupada no cuenta a los que aun no hemos colocado
	p->zx = rand() % COLUMNAS;
	p->zy = rand() % FILAS;
	for (k = 0; k < p->num; k++)
	{
		do
		{
			x = rand() % COLUMNAS;
			y = rand() % FILAS;
		} while (Ocupada(p, x, y) || Distancia(x, y, p->zx, p->zy) < 6);
		p->x[k] = x;
		p->y[k] = y;
		p->vivo[k] = 1;
	}
	p->turno = 0;
	p->ronda = 1;
}

void EnviarEstado(int id)
{
	//Manda a todos el tablero: 9/id/turno/zx/zy/n/(nombre/x/y/vivo)...
	Partida *p = &partidas[id];
	char mensaje[1024], trozo[100];
	int k;
	sprintf(mensaje, "9/%d/%s/%d/%d/%d", id, p->nombre[p->turno], p->zx, p->zy, p->num);
	for (k = 0; k < p->num; k++)
	{
		sprintf(trozo, "/%s/%d/%d/%d", p->nombre[k], p->x[k], p->y[k], p->vivo[k]);
		strcat(mensaje, trozo);
	}
	EnviarAPartida(p, "%s", mensaje);
}

void MueveZombie(int id)
{
	//El zombie va a por el jugador vivo mas cercano. Puede moverse a cualquier casilla del rombo (a 1 o 2 pasos:
	//las 8 de alrededor mas las 4 que estan a dos casillas en linea recta). Si cae encima de un jugador, lo mata
	Partida *p = &partidas[id];
	int k, dx, dy;

	//1. Buscamos el jugador vivo mas cercano
	int objetivo = -1;
	int distanciaMin = 1000000;
	for (k = 0; k < p->num; k++)
	{
		if (p->vivo[k] && Distancia(p->zx, p->zy, p->x[k], p->y[k]) < distanciaMin)
		{
			distanciaMin = Distancia(p->zx, p->zy, p->x[k], p->y[k]);
			objetivo = k;
		}
	}
	if (objetivo == -1)
	{ return; }

	//2. De las casillas del rombo nos quedamos con la que deje al zombie mas cerca de el
	int mejorX = p->zx, mejorY = p->zy;
	int mejorDistancia = distanciaMin;
	for (dy = -2; dy <= 2; dy++)
	{
		for (dx = -2; dx <= 2; dx++)
		{
			int nx = p->zx + dx;
			int ny = p->zy + dy;
			if (abs(dx) + abs(dy) == 0 || abs(dx) + abs(dy) > 2)
			{ continue; }	//Fuera del rombo
			if (nx < 0 || nx >= COLUMNAS || ny < 0 || ny >= FILAS)
			{ continue; }	//Fuera del tablero
			if (Distancia(nx, ny, p->x[objetivo], p->y[objetivo]) < mejorDistancia)
			{
				mejorDistancia = Distancia(nx, ny, p->x[objetivo], p->y[objetivo]);
				mejorX = nx;
				mejorY = ny;
			}
		}
	}
	p->zx = mejorX;
	p->zy = mejorY;

	//3. Si ha caido encima de un jugador, lo mata
	for (k = 0; k < p->num; k++)
	{
		if (p->vivo[k] && p->x[k] == p->zx && p->y[k] == p->zy)
		{
			p->vivo[k] = 0;
			EnviarAPartida(p, "12/El zombie ha atrapado a %s", p->nombre[k]);
		}
	}
}

void TerminaPartida(int id, int ganan)
{
	//Fin de la partida: avisamos a todos (10/id/Victoria o 10/id/Derrota) y liberamos el hueco
	//(aqui es donde guardaremos la partida en la base de datos en las proximas versiones)
	EnviarAPartida(&partidas[id], "10/%d/%s", id, ganan ? "Victoria" : "Derrota");
	partidas[id].estado = LIBRE;
}

void AvanzaTurno(int id)
{
	//Pasa el turno al siguiente jugador vivo. Si ya han jugado todos, mueve el zombie y empieza otra ronda
	Partida *p = &partidas[id];
	int k;
	for (k = p->turno + 1; k < p->num; k++)
	{
		if (p->vivo[k])
		{
			p->turno = k;
			EnviarEstado(id);
			return;
		}
	}

	//Han jugado todos: le toca al zombie
	MueveZombie(id);
	p->turno = 0;
	for (k = p->num - 1; k >= 0; k--)
	{
		if (p->vivo[k])
		{ p->turno = k; }	//Nos quedamos con el primer vivo
	}
	EnviarEstado(id);		//Asi todos ven el tablero final

	if (ContarVivos(p) == 0)
	{ TerminaPartida(id, 0); }
	else if (RONDAS_PARA_GANAR > 0 && p->ronda >= RONDAS_PARA_GANAR)
	{ TerminaPartida(id, 1); }
	else
	{ p->ronda++; }
}

void SaleJugador(char *nombre)
{
	//Un cliente se ha desconectado: lo sacamos de su partida (si tenia)
	int id = BuscaPartidaDeJugador(nombre);
	if (id == -1)
	{ return; }
	Partida *p = &partidas[id];
	int pos = DamePosicionEnPartida(p, nombre);
	p->socket[pos] = -1;	//Ya no podemos escribirle

	if (p->estado != JUGANDO)
	{
		//Estaba invitando o invitado: la invitacion se cancela (lo contamos como un rechazo)
		EnviarAPartida(p, "8/%d/No/%s", id, nombre);
		p->estado = LIBRE;
		return;
	}
	//Estaba jugando: lo damos por muerto y si era su turno pasamos al siguiente
	p->vivo[pos] = 0;
	EnviarAPartida(p, "12/%s se ha desconectado", nombre);
	if (ContarVivos(p) == 0)
	{ TerminaPartida(id, 0); }
	else if (p->turno == pos)
	{ AvanzaTurno(id); }
	else
	{ EnviarEstado(id); }
}

/* ---------------- 7. PETICIONES DE LOS CLIENTES ---------------- */

int ContarFilas(char *consulta)
{
	//Ejecuta una consulta y devuelve cuantas filas da (-1 si hay error)
	if (mysql_query(conn, consulta) != 0)
	{ return -1; }
	MYSQL_RES *resultado = mysql_store_result(conn);
	if (resultado == NULL)
	{ return 0; }
	int filas = mysql_num_rows(resultado);
	mysql_free_result(resultado);
	return filas;
}

void Escapa(char *origen, char *destino)
{
	//Para evitar inyeccion SQL. destino tiene que medir como minimo 2*strlen(origen)+1
	mysql_real_escape_string(conn, destino, origen, strlen(origen));
}

int SeparaUsuarioContra(char *datos, char **usuario, char **con)
{
	//Divide "usuario-contrasena" por el primer guion. 1 si esta bien, 0 si no
	char *guion = strchr(datos, '-');
	if (guion == NULL)
	{ return 0; }
	*guion = '\0';
	*usuario = datos;
	*con = guion + 1;
	if (strlen(*usuario) < 1 || strlen(*usuario) > 19 || strchr(*usuario, '/') != NULL)
	{ return 0; }
	if (strlen(*con) < 1 || strlen(*con) > 29)
	{ return 0; }
	return 1;
}

void AtiendeRegistro(int sock, char *resto)
{
	//Peticion 1/usuario-contrasena
	char *usuario, *con;
	char uEsc[60], cEsc[100], consulta[300];
	if (!SeparaUsuarioContra(resto, &usuario, &con))
	{ Enviar(sock, "1/NO/Usuario o contraseña no válidos (usuario de 1 a 19 caracteres, sin barras)"); return; }
	Escapa(usuario, uEsc);
	Escapa(con, cEsc);

	//Antes de anadir comprobamos que el nombre sea distinto y no haya duplicados
	sprintf(consulta, "SELECT PlayerTable.Name FROM PlayerTable WHERE PlayerTable.Name = '%s'", uEsc);
	int filas = ContarFilas(consulta);
	if (filas == -1)
	{ Enviar(sock, "1/NO/Error al consultar la base de datos"); return; }
	if (filas > 0)
	{ Enviar(sock, "1/NO/Nombre de usuario ya en uso"); return; }

	sprintf(consulta, "INSERT INTO PlayerTable (Name, Contra, GlobalPoints) VALUES ('%s','%s',0)", uEsc, cEsc);
	if (mysql_query(conn, consulta) != 0)
	{ Enviar(sock, "1/NO/Error al registrar el usuario"); }
	else
	{ Enviar(sock, "1/OK/Usuario registrado correctamente, ya puedes iniciar sesión"); }
}

void AtiendeLogin(int sock, char *usuarioSesion, char *resto)
{
	//Peticion 2/usuario-contrasena. Si todo va bien guardamos el nombre en usuarioSesion (el nombre de este cliente)
	char *usuario, *con;
	char uEsc[60], cEsc[100], consulta[300];
	if (usuarioSesion[0] != '\0')
	{ Enviar(sock, "2/NO/Ya has iniciado sesión como %s", usuarioSesion); return; }
	if (!SeparaUsuarioContra(resto, &usuario, &con))
	{ Enviar(sock, "2/NO/Usuario o contraseña no válidos"); return; }
	Escapa(usuario, uEsc);
	Escapa(con, cEsc);

	sprintf(consulta, "SELECT PlayerTable.Name FROM PlayerTable WHERE PlayerTable.Name = '%s' AND PlayerTable.Contra = '%s'", uEsc, cEsc);
	int filas = ContarFilas(consulta);
	if (filas == -1)
	{ Enviar(sock, "2/NO/Error en la base de datos"); }
	else if (filas == 0)
	{ Enviar(sock, "2/NO/Usuario o contraseña incorrectos"); }
	else if (DamePosicion(&miListaConectados, usuario) != -1)
	{ Enviar(sock, "2/NO/Este usuario ya está conectado desde otro cliente"); }
	else if (PonConectado(&miListaConectados, usuario, sock) == -1)
	{ Enviar(sock, "2/NO/El servidor está lleno"); }
	else
	{
		strcpy(usuarioSesion, usuario);
		Enviar(sock, "2/Correcto/Inicio de sesión correcto");
		NotificaConectados();	//La lista ha cambiado: se la mandamos a todos
	}
}

void AtiendeConsulta(int sock, int codigo, char *resto)
{
	//Peticiones 3, 4 y 5 (consultas a la base de datos). Preparamos la consulta segun el codigo y la lanzamos
	char param[31], esc[70], consulta[500];
	strncpy(param, resto, 30);
	param[30] = '\0';
	Escapa(param, esc);

	if (codigo == 3)		//3/jugador -> ¿ha encontrado pistola?
	{
		sprintf(consulta,
			"SELECT PartidaTable.Code FROM PlayerTable, PartidaTable, Relation "
			"WHERE PlayerTable.Name = '%s' AND PlayerTable.ID = Relation.PlayerID "
			"AND Relation.PartidaCode = PartidaTable.Code "
			"AND Relation.ZombieKilled = 1 AND PartidaTable.PistolaFound = 1", esc);
	}
	else if (codigo == 4)	//4/partida -> numero de jugadores de la partida
	{ sprintf(consulta, "SELECT COUNT(*) FROM Relation WHERE PartidaCode = '%s'", esc); }
	else					//5/partida -> jugador que mas tiempo sobrevivio en la partida
	{
		sprintf(consulta,
			"SELECT PlayerTable.Name, Relation.TiempoSobrevivido FROM PlayerTable, Relation "
			"WHERE PlayerTable.ID = Relation.PlayerID AND Relation.PartidaCode = '%s' "
			"ORDER BY Relation.TiempoSobrevivido DESC LIMIT 1", esc);
	}

	if (mysql_query(conn, consulta) != 0)
	{ Enviar(sock, "%d/ERROR/Error al consultar la base de datos", codigo); return; }
	MYSQL_RES *resultado = mysql_store_result(conn);
	MYSQL_ROW row = NULL;
	if (resultado != NULL)
	{ row = mysql_fetch_row(resultado); }		//Cogemos la primera fila (si hay)

	if (codigo == 3 && row == NULL)
	{ Enviar(sock, "3/NO/El jugador %s no ha encontrado o usado pistola.", param); }
	else if (codigo == 3)
	{ Enviar(sock, "3/SI/Pistola encontrada."); }
	else if (codigo == 4)
	{ Enviar(sock, "4/%s", row != NULL ? row[0] : "0"); }
	else if (row == NULL)
	{ Enviar(sock, "5/ERROR/No existe la partida %s o no tiene jugadores", param); }
	else
	{ Enviar(sock, "5/%s-%s", row[0], row[1]); }

	if (resultado != NULL)
	{ mysql_free_result(resultado); }
}

/* ---------- PROTOCOLO DE INVITACION (version 4) ---------- */

void AtiendeInvitacion(int sock, char *usuario, char *resto)
{
	//Peticion 7/invitado1/invitado2/...  La manda el anfitrion.
	//Creamos una partida con el anfitrion y todos los invitados (que empiezan con la respuesta pendiente)
	//y le mandamos la invitacion a cada invitado. Si algo falla no se crea nada.
	char *saveptr, *error = NULL;
	int k;
	if (BuscaPartidaDeJugador(usuario) != -1)
	{ Enviar(sock, "11/Ya estás en una partida (o esperando respuestas a una invitación)"); return; }
	int id = CreaPartida();
	if (id == -1)
	{ Enviar(sock, "11/El servidor no admite más partidas ahora mismo"); return; }
	Partida *p = &partidas[id];
	strcpy(p->nombre[0], usuario);		//El anfitrion es el jugador 0 y ya "ha aceptado"
	p->socket[0] = sock;
	p->respuesta[0] = 1;
	p->num = 1;

	//Recorremos los invitados uno a uno
	char *invitado = strtok_r(resto, "/", &saveptr);
	while (invitado != NULL && error == NULL)
	{
		int pos = DamePosicion(&miListaConectados, invitado);
		if (strcmp(invitado, usuario) == 0)
		{ error = "No puedes invitarte a ti mismo"; }
		else if (pos == -1)
		{ error = "Uno de los jugadores ya no está conectado"; }
		else if (BuscaPartidaDeJugador(invitado) != -1)
		{ error = "Uno de los jugadores ya está en otra partida o tiene una invitación pendiente"; }
		else if (p->num == MAX_JUG)
		{ error = "Demasiados jugadores (el máximo es 6 contando contigo)"; }
		else
		{
			//Todo bien: lo anadimos a la partida con la respuesta pendiente
			strcpy(p->nombre[p->num], invitado);
			p->socket[p->num] = miListaConectados.conectados[pos].socket;
			p->respuesta[p->num] = 0;
			p->num++;
		}
		invitado = strtok_r(NULL, "/", &saveptr);
	}
	if (error == NULL && p->num == 1)
	{ error = "Selecciona al menos un jugador para invitar"; }
	if (error != NULL)
	{
		Enviar(sock, "11/%s", error);
		p->estado = LIBRE;		//Cancelamos todo
		return;
	}

	//Mandamos la invitacion a cada invitado: 7/id/anfitrion
	for (k = 1; k < p->num; k++)
	{ Enviar(p->socket[k], "7/%d/%s", id, usuario); }
	Enviar(sock, "12/Invitación enviada a %d jugador(es), esperando respuestas...", p->num - 1);
}

void ComprobarRespuestas(int id)
{
	//Se llama cada vez que un invitado responde. Cuando ya han respondido todos, decidimos si se juega o no
	//y se lo decimos a TODOS (anfitrion e invitados):  8/id/Si/jugadores...   o   8/id/No/rechazan...
	Partida *p = &partidas[id];
	int k, pendientes = 0, rechazos = 0;
	char lista[300] = "";
	for (k = 1; k < p->num; k++)
	{
		if (p->respuesta[k] == 0)
		{ pendientes++; }
		if (p->respuesta[k] == 2)
		{ rechazos++; }
	}
	if (pendientes > 0)
	{ return; }		//Todavia faltan respuestas

	if (rechazos == 0)
	{
		//Todos han aceptado: la partida se jugara. Mandamos todos los nombres (el primero es el anfitrion)
		p->estado = LISTA;
		for (k = 0; k < p->num; k++)
		{
			strcat(lista, "/");
			strcat(lista, p->nombre[k]);
		}
		EnviarAPartida(p, "8/%d/Si%s", id, lista);
	}
	else
	{
		//Alguien ha rechazado: no se jugara. Mandamos los nombres de los que han rechazado
		for (k = 1; k < p->num; k++)
		{
			if (p->respuesta[k] == 2)
			{
				strcat(lista, "/");
				strcat(lista, p->nombre[k]);
			}
		}
		EnviarAPartida(p, "8/%d/No%s", id, lista);
		p->estado = LIBRE;		//Se acabo la partida
	}
}

void AtiendeRespuesta(int sock, char *usuario, char *resto)
{
	//Peticion 8/id/Si  o  8/id/No  (respuesta de un invitado)
	int id = -1;
	char resp[10] = "";
	sscanf(resto, "%d/%9s", &id, resp);
	if (id < 0 || id >= MAX_PARTIDAS || partidas[id].estado != INVITANDO)
	{ Enviar(sock, "11/Esa invitación ya no está disponible"); return; }
	Partida *p = &partidas[id];
	int pos = DamePosicionEnPartida(p, usuario);
	if (pos < 1 || p->respuesta[pos] != 0)
	{ Enviar(sock, "11/No tienes ninguna invitación pendiente en esa partida"); return; }

	if (strcmp(resp, "Si") == 0)
	{
		p->respuesta[pos] = 1;
		Enviar(p->socket[0], "12/%s ha aceptado la invitación", usuario);		//Aviso al anfitrion
	}
	else
	{
		p->respuesta[pos] = 2;
		Enviar(p->socket[0], "12/%s ha rechazado la invitación", usuario);
	}
	ComprobarRespuestas(id);
}

/* ---------- JUEGO ---------- */

void AtiendeInicio(int sock, char *usuario, char *resto)
{
	//Peticion 9/id  (el anfitrion inicia la partida cuando todos han aceptado)
	int id = atoi(resto);
	if (id < 0 || id >= MAX_PARTIDAS || partidas[id].estado != LISTA)
	{ Enviar(sock, "11/Esa partida no está lista para empezar"); return; }
	if (strcmp(partidas[id].nombre[0], usuario) != 0)
	{ Enviar(sock, "11/Solo el anfitrión (%s) puede iniciar la partida", partidas[id].nombre[0]); return; }

	IniciaPartida(&partidas[id]);
	partidas[id].estado = JUGANDO;
	EnviarAPartida(&partidas[id], "12/¡Empieza la partida! Sobrevivid %d rondas", RONDAS_PARA_GANAR);
	EnviarEstado(id);
}

void AtiendeMovimiento(int sock, char *usuario, char *resto)
{
	//Peticion 10/id/x/y  (mover al jugador a la casilla x, y)
	int id = -1, x = -1, y = -1;
	sscanf(resto, "%d/%d/%d", &id, &x, &y);
	if (id < 0 || id >= MAX_PARTIDAS || partidas[id].estado != JUGANDO)
	{ Enviar(sock, "11/No hay ninguna partida en marcha"); return; }
	Partida *p = &partidas[id];
	int k = p->turno;
	if (strcmp(p->nombre[k], usuario) != 0)
	{ Enviar(sock, "11/No es tu turno, juega %s", p->nombre[k]); return; }
	if (x < 0 || x >= COLUMNAS || y < 0 || y >= FILAS)
	{ Enviar(sock, "11/Esa casilla está fuera del tablero"); return; }
	if (Distancia(p->x[k], p->y[k], x, y) != 1)
	{ Enviar(sock, "11/Solo puedes moverte a una casilla adyacente (arriba, abajo, izquierda o derecha)"); return; }
	if (Ocupada(p, x, y))
	{ Enviar(sock, "11/Esa casilla está ocupada"); return; }

	p->x[k] = x;
	p->y[k] = y;
	AvanzaTurno(id);
}

void AtiendeChat(int sock, char *usuario, char *resto)
{
	//Peticion 13/texto  ->  se lo reenviamos a todos los de nuestra partida como 13/nombre/texto
	int id = BuscaPartidaDeJugador(usuario);
	if (id == -1)
	{ Enviar(sock, "11/Para usar el chat tienes que estar en una partida"); return; }
	EnviarAPartida(&partidas[id], "13/%s/%s", usuario, resto);
}

void AtiendeMensaje(int sock, char *usuario, char *linea, int *terminar)
{
	//Recibe UN mensaje ("codigo/datos") y llama a la funcion que lo atiende
	if (linea[0] < '0' || linea[0] > '9')
	{ Enviar(sock, "11/Mensaje no válido"); return; }
	int codigo = atoi(linea);
	char *resto = strchr(linea, '/');	//Los datos son lo que va despues de la primera barra
	if (resto != NULL)
	{ resto++; }
	else
	{ resto = linea + strlen(linea); }
	printf("Peticion de %s -> Codigo: %d, Datos: %s\n", usuario[0] ? usuario : "(sin sesion)", codigo, resto);

	//Todo lo que sea invitar o jugar necesita haber iniciado sesion
	if (codigo >= 7 && usuario[0] == '\0')
	{ Enviar(sock, "11/Inicia sesión primero"); return; }

	switch (codigo)
	{
		case 0: *terminar = 1; break;								//Desconexion
		case 1: AtiendeRegistro(sock, resto); break;
		case 2: AtiendeLogin(sock, usuario, resto); break;
		case 3:
		case 4:
		case 5: AtiendeConsulta(sock, codigo, resto); break;
		case 7: AtiendeInvitacion(sock, usuario, resto); break;
		case 8: AtiendeRespuesta(sock, usuario, resto); break;
		case 9: AtiendeInicio(sock, usuario, resto); break;
		case 10: AtiendeMovimiento(sock, usuario, resto); break;
		case 13: AtiendeChat(sock, usuario, resto); break;
		default: Enviar(sock, "11/Petición desconocida (código %d)", codigo);
	}
}

/* ---------------- 8. THREAD DE CADA CLIENTE ---------------- */

void *AtenderCliente(void *arguments)
{
	int sock_conn = *(int *)arguments;
	free(arguments);
	char usuario[20] = "";		//Nombre de este cliente (vacio hasta que inicie sesion)
	char peticion[1024];
	int terminar = 0;

	// Entramos en un bucle para atender todas las peticiones de este cliente hasta que se desconecte
	while (terminar == 0)
	{
		int ret = read(sock_conn, peticion, sizeof(peticion) - 1);
		if (ret <= 0)
		{ break; }		//El cliente se ha ido (cerrado o caido)
		peticion[ret] = '\0';

		//Pueden llegar varios mensajes pegados: los separamos por '\n' y atendemos uno a uno
		char *saveptr;
		char *linea = strtok_r(peticion, "\n", &saveptr);
		while (linea != NULL && terminar == 0)
		{
			pthread_mutex_lock(&mutex);		//Para que nadie toque nuestras listas mientras nosotros las tocamos
			AtiendeMensaje(sock_conn, usuario, linea, &terminar);
			pthread_mutex_unlock(&mutex);	//Ya pueden toquetear otros
			linea = strtok_r(NULL, "\n", &saveptr);
		}
	}

	//Se acabo el servicio para este cliente: lo sacamos de su partida y de los conectados y avisamos a todos
	pthread_mutex_lock(&mutex);
	if (usuario[0] != '\0')
	{
		SaleJugador(usuario);
		EliminaConectado(&miListaConectados, usuario);
		NotificaConectados();
		printf("Eliminado %s\n", usuario);
	}
	pthread_mutex_unlock(&mutex);
	close(sock_conn);
	return NULL;
}

/* ---------------- 9. MAIN ---------------- */

int main(int argc, char *argv[])
{
	signal(SIGPIPE, SIG_IGN);	//Si un cliente se cae, que no muera el servidor al escribirle
	srand(time(NULL));

	/*MySQL*/
	//Crear conexion al servidor MySQL
	conn = mysql_init(NULL);
	if (conn == NULL)
	{
		printf("Error al crear conexion: %u %s\n", mysql_errno(conn), mysql_error(conn));
		exit(1);
	}
	//Conectamos con lo creado								Tabla
	conn = mysql_real_connect(conn, "localhost", "root", "mysql", "Zombie", 0, NULL, 0);
	if (conn == NULL)	//Proteccion
	{
		printf("Error al iniciar la conexion con MySQL (esta arrancado? existe la base Zombie? la contrasena es 'mysql'?)\n");
		exit(1);
	}

	/*Conexion servidor*/
	int sock_conn, sock_listen;
	struct sockaddr_in serv_adr;

	// Abrimos el socket
	if ((sock_listen = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		printf("Error creant socket\n");
		exit(1);
	}
	int si = 1;
	setsockopt(sock_listen, SOL_SOCKET, SO_REUSEADDR, &si, sizeof(si));	//Para poder reiniciar el servidor sin esperar

	// Hacemos el bind al port
	memset(&serv_adr, 0, sizeof(serv_adr));	// inicialitza a zero serv_addr
	serv_adr.sin_family = AF_INET;
	// asocia el socket a cualquiera de las IP de la maquina. htonl formatea el numero que recibe al formato necesario
	serv_adr.sin_addr.s_addr = htonl(INADDR_ANY);
	// establecemos el puerto de escucha
	serv_adr.sin_port = htons(PUERTO);
	if (bind(sock_listen, (struct sockaddr *)&serv_adr, sizeof(serv_adr)) < 0)
	{
		printf("Error al bind (hay otro servidor abierto en el puerto %d?)\n", PUERTO);
		exit(1);
	}
	if (listen(sock_listen, 10) < 0)
	{
		printf("Error en el Listen\n");
		exit(1);
	}

	for (;;)
	{
		printf("Escuchando\n");
		sock_conn = accept(sock_listen, NULL, NULL);
		if (sock_conn < 0)
		{ continue; }
		printf("He recibido conexion\n");

		//Le pasamos al thread el socket de este cliente (en memoria nueva para que cada thread tenga el suyo)
		int *socketCliente = malloc(sizeof(int));
		*socketCliente = sock_conn;

		// Crear thread y decirle lo que tiene que hacer
		pthread_t thread;
		pthread_create(&thread, NULL, AtenderCliente, socketCliente);
		pthread_detach(thread);
	}

	mysql_close(conn);	//Cerrar conexion
	return 0;
}
