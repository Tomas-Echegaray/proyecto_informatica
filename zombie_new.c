/*
 * ServidorZombie.c  -  Servidor del juego Zombie Apocalipsis
 *
 * Compilar en la VM:
 *   gcc ServidorZombie.c -o servidor $(mysql_config --cflags --libs) -lpthread
 * Ejecutar:
 *   ./servidor
 *
 * PROTOCOLO (cada mensaje termina en '\n'; las partes se separan con '/')
 *
 * CLIENTE -> SERVIDOR
 *   0/                      Desconexion
 *   1/usuario-contraseña    Registro
 *   2/usuario-contraseña    Inicio de sesion
 *   3/jugador               Consulta: ¿ha encontrado pistola?
 *   4/codigoPartida         Consulta: numero de jugadores de la partida
 *   5/codigoPartida         Consulta: jugador que mas tiempo sobrevivio
 *   7/invitado              Invitar a un jugador conectado a mi lobby
 *   9/codigo/Aceptado|Rechazado   Respuesta a una invitacion
 *   10/codigo               Iniciar partida (solo el anfitrion)
 *   12/codigo/x/y           Mover mi jugador a la casilla (x, y)
 *   16/texto                Chat del lobby
 *
 * SERVIDOR -> CLIENTE
 *   1/OK|NO/mensaje                     Resultado del registro
 *   2/Correcto|NO/mensaje               Resultado del login
 *   3/SI|NO|ERROR/mensaje               Consulta pistola
 *   4/numero                            Consulta numero de jugadores
 *   5/jugador-tiempo                    Consulta jugador mas longevo
 *   6/n/nombre1/nombre2/...             Lista de conectados (se envia sola)
 *   7/codigo/jugador/Aceptado|Rechazado Cambio en el lobby
 *   8/codigo/anfitrion                  Te han invitado a una partida
 *   10/codigo/turno/zx/zy/n/nombre/x/y/vivo/...   Estado del tablero
 *   13/codigoViejo/Victoria|Derrota/codigoNuevo   Fin de partida
 *   14/mensaje                          Error (MessageBox en el cliente)
 *   15/mensaje                          Informacion (label en el cliente)
 *   16/jugador/texto                    Mensaje de chat
 *   17/codigo/jugador                   Un jugador ha salido del lobby
 */

#define _GNU_SOURCE
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

#include <mysql.h>	//MySQL

//Constantes del juego (COLUMNAS y FILAS deben coincidir con el tablero del cliente)
#define PUERTO 9080
#define COLUMNAS 30
#define FILAS 34
#define MAX_JUGADORES 6
#define MAX_PARTIDAS 50
#define MIN_JUGADORES 2			//Segun el diagrama serian 3; ponlo en 2 para probar con 2 clientes
#define RONDAS_PARA_GANAR 20	//Rondas que hay que sobrevivir para ganar (0 = no se gana nunca, hasta que haya pistola)

//Estructuras
typedef struct {
	//Estructura para unir conexion de SQL y la de socket para pasarsela a la funcion de AtenderCliente
	int sock_conn;
	MYSQL *conn;
} Datos;

typedef struct {
	//Estructura de cliente conectado
	char nombre[20];
	int socket;
	char invitacion[10];	//Codigo de la partida a la que le han invitado ("" si no tiene ninguna pendiente)
} Conectado;

typedef struct {
	//Lista de clientes conectados
	Conectado conectados[100];
	int num;
} ListaConectados;

typedef struct {
	//Una partida (lobby + juego). El jugador 0 es el anfitrion
	int activa;				//1 si el hueco esta en uso
	char codigo[10];
	int iniciada;			//1 si se esta jugando
	int num;				//Jugadores en la partida
	char nombre[MAX_JUGADORES][20];
	int socket[MAX_JUGADORES];	//-1 si se ha desconectado en mitad de la partida
	int vivo[MAX_JUGADORES];
	int x[MAX_JUGADORES];
	int y[MAX_JUGADORES];
	time_t tMuerte[MAX_JUGADORES];
	int zx, zy;				//Posicion del zombie
	int turno;				//Indice del jugador al que le toca
	int ronda;
	time_t tInicio;
} Partida;

//Estructura necesaria para acceso excluyente (protege TODO: lista de conectados, partidas y la conexion SQL)
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

ListaConectados miListaConectados = { .num = 0 };
Partida partidas[MAX_PARTIDAS];
long contadorCodigo = 0;

//Para que no se queje de que las funciones estan definidas despues
int PonConectado(ListaConectados *lista, char nombre[20], int socket);
int DamePosicion(ListaConectados *lista, char nombre[20]);
int EliminaConectado(ListaConectados *lista, char nombre[20]);
void DameConectados(ListaConectados *lista, char conectados[2300]);
void TerminaPartida(Partida *p, MYSQL *conn, int ganan);
void AvanzaTurno(Partida *p, MYSQL *conn);
void EnviarEstado(Partida *p);

/* ---------------------------------------------------------------- */
/* Utilidades de red                                                */
/* ---------------------------------------------------------------- */

void Enviar(int sock, const char *formato, ...)
{
	//Envia un mensaje terminado en '\n' (asi el cliente puede separar mensajes pegados)
	char buffer[4096];
	va_list args;
	va_start(args, formato);
	int n = vsnprintf(buffer, sizeof(buffer), formato, args);
	va_end(args);
	if (n < 0) return;
	if (n >= (int)sizeof(buffer) - 1) n = sizeof(buffer) - 2;
	buffer[n] = '\n';
	send(sock, buffer, n + 1, MSG_NOSIGNAL);
}

void EnviaAPartida(Partida *p, const char *formato, ...)
{
	char mensaje[2048];
	va_list args;
	va_start(args, formato);
	vsnprintf(mensaje, sizeof(mensaje), formato, args);
	va_end(args);
	int k;
	for (k = 0; k < p->num; k++)
	{
		if (p->socket[k] >= 0)
			Enviar(p->socket[k], "%s", mensaje);
	}
}

void NotificaConectados()
{
	//Manda a todos los conectados la lista actualizada
	char lista[2300];
	char notificacion[2400];
	DameConectados(&miListaConectados, lista);
	sprintf(notificacion, "6/%s", lista);
	int j;
	for (j = 0; j < miListaConectados.num; j++)
		Enviar(miListaConectados.conectados[j].socket, "%s", notificacion);
}

/* ---------------------------------------------------------------- */
/* Lista de conectados                                              */
/* ---------------------------------------------------------------- */

int PonConectado(ListaConectados *lista, char nombre[20], int socket)
{
	//Anade un nuevo conectado. 0 chill, -1 si llena y no pudo anadir
	if (lista->num == 100)
	{ return -1; }
	else
	{
		strcpy(lista->conectados[lista->num].nombre, nombre);
		lista->conectados[lista->num].socket = socket;
		lista->conectados[lista->num].invitacion[0] = '\0';
		lista->num++;
		return 0;
	}
}

int DamePosicion(ListaConectados *lista, char nombre[20])
{
	//Devuelve la posicion en la lista (0) o -1 si no lo encuentra. Busqueda
	int i = 0;
	int encontrado = 0;
	while ((i < lista->num) && !encontrado)
	{
		if (strcmp(lista->conectados[i].nombre, nombre) == 0)
		{ encontrado = 1; }
		if (!encontrado)
		{ i++; }
	}
	if (encontrado)
	{ return i; }
	else
	{ return -1; }
}

int EliminaConectado(ListaConectados *lista, char nombre[20])
{
	//0 si elimina, -1 si no existe
	int posicion = DamePosicion(lista, nombre);
	if (posicion == -1)
	{ return -1; }
	else
	{
		int i;
		for (i = posicion; i < lista->num - 1; i++)
		{
			lista->conectados[i] = lista->conectados[i + 1];
		}
		lista->num--;
		return 0;
	}
}

void DameConectados(ListaConectados *lista, char conectados[2300])
{
	//Rellena "n/nombre1/nombre2/..." (primero va el numero de conectados)
	int len = sprintf(conectados, "%d", lista->num);
	int i;
	for (i = 0; i < lista->num; i++)
	{
		len += sprintf(conectados + len, "/%s", lista->conectados[i].nombre);
	}
}

/* ---------------------------------------------------------------- */
/* Partidas                                                         */
/* ---------------------------------------------------------------- */

void NuevoCodigo(char *destino)
{
	sprintf(destino, "G%ld", contadorCodigo++);
}

int BuscaPartidaPorCodigo(const char *codigo)
{
	int i;
	for (i = 0; i < MAX_PARTIDAS; i++)
	{
		if (partidas[i].activa && strcmp(partidas[i].codigo, codigo) == 0)
			return i;
	}
	return -1;
}

int BuscaPartidaDeJugador(const char *nombre, int *pos)
{
	//Devuelve el indice de la partida en la que esta el jugador (y su posicion dentro), o -1
	int i, k;
	for (i = 0; i < MAX_PARTIDAS; i++)
	{
		if (!partidas[i].activa) continue;
		for (k = 0; k < partidas[i].num; k++)
		{
			if (strcmp(partidas[i].nombre[k], nombre) == 0)
			{
				*pos = k;
				return i;
			}
		}
	}
	return -1;
}

int CreaPartida(const char *anfitrion, int socket)
{
	int i;
	for (i = 0; i < MAX_PARTIDAS; i++)
	{
		if (!partidas[i].activa)
		{
			memset(&partidas[i], 0, sizeof(Partida));
			partidas[i].activa = 1;
			NuevoCodigo(partidas[i].codigo);
			partidas[i].num = 1;
			strcpy(partidas[i].nombre[0], anfitrion);
			partidas[i].socket[0] = socket;
			return i;
		}
	}
	return -1;
}

void EliminaMiembro(Partida *p, int pos)
{
	//Saca a un jugador del lobby y avisa a los demas
	EnviaAPartida(p, "17/%s/%s", p->codigo, p->nombre[pos]);
	int k;
	for (k = pos; k < p->num - 1; k++)
	{
		strcpy(p->nombre[k], p->nombre[k + 1]);
		p->socket[k] = p->socket[k + 1];
		p->vivo[k] = p->vivo[k + 1];
		p->x[k] = p->x[k + 1];
		p->y[k] = p->y[k + 1];
		p->tMuerte[k] = p->tMuerte[k + 1];
	}
	p->num--;
	if (p->num == 0)
		p->activa = 0;
}

void LimpiaDesconectados(Partida *p)
{
	//Tras acabar una partida, quitamos del lobby a los que se desconectaron durante el juego
	int k;
	for (k = p->num - 1; k >= 0; k--)
	{
		if (p->socket[k] < 0)
			EliminaMiembro(p, k);
	}
}

/* ---------------------------------------------------------------- */
/* Logica del juego                                                 */
/* ---------------------------------------------------------------- */

int Distancia(int x1, int y1, int x2, int y2)
{
	return abs(x1 - x2) + abs(y1 - y2);
}

int ContarVivos(Partida *p)
{
	int k, vivos = 0;
	for (k = 0; k < p->num; k++)
		if (p->vivo[k]) vivos++;
	return vivos;
}

int Ocupada(Partida *p, int x, int y)
{
	//1 si hay el zombie o un jugador vivo en esa casilla
	if (p->zx == x && p->zy == y) return 1;
	int k;
	for (k = 0; k < p->num; k++)
	{
		if (p->vivo[k] && p->x[k] == x && p->y[k] == y)
			return 1;
	}
	return 0;
}

void IniciaPartida(Partida *p)
{
	//Spawn aleatorio del zombie y de los jugadores (lejos del zombie para no morir en el primer turno)
	int k;
	for (k = 0; k < p->num; k++)
	{
		p->vivo[k] = 0;
		p->tMuerte[k] = 0;
	}
	p->zx = rand() % COLUMNAS;
	p->zy = rand() % FILAS;
	for (k = 0; k < p->num; k++)
	{
		int x, y;
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
	p->tInicio = time(NULL);
	p->iniciada = 1;
}

void EnviarEstado(Partida *p)
{
	//10/codigo/turno/zx/zy/n/nombre/x/y/vivo/...
	char mensaje[1024];
	int len = sprintf(mensaje, "10/%s/%s/%d/%d/%d", p->codigo, p->nombre[p->turno], p->zx, p->zy, p->num);
	int k;
	for (k = 0; k < p->num; k++)
	{
		len += sprintf(mensaje + len, "/%s/%d/%d/%d", p->nombre[k], p->x[k], p->y[k], p->vivo[k]);
	}
	EnviaAPartida(p, "%s", mensaje);
}

void MueveZombie(Partida *p)
{
	//El zombie persigue al jugador vivo mas cercano. Puede moverse a las casillas del rombo
	//(a distancia 1 o 2 en Manhattan: las 8 de alrededor mas 2 en linea recta). Si cae sobre un jugador, lo mata.
	int objetivo = -1;
	int dmin = 1000000;
	int k;
	for (k = 0; k < p->num; k++)
	{
		if (!p->vivo[k]) continue;
		int d = Distancia(p->zx, p->zy, p->x[k], p->y[k]);
		if (d < dmin)
		{
			dmin = d;
			objetivo = k;
		}
	}
	if (objetivo == -1) return;

	int ox = p->x[objetivo];
	int oy = p->y[objetivo];
	int mejorX = p->zx;
	int mejorY = p->zy;
	int mejorD = dmin;
	int dx, dy;
	for (dy = -2; dy <= 2; dy++)
	{
		for (dx = -2; dx <= 2; dx++)
		{
			int pasos = abs(dx) + abs(dy);
			if (pasos == 0 || pasos > 2) continue;	//Fuera del rombo
			int nx = p->zx + dx;
			int ny = p->zy + dy;
			if (nx < 0 || nx >= COLUMNAS || ny < 0 || ny >= FILAS) continue;
			int d = Distancia(nx, ny, ox, oy);
			if (d < mejorD)
			{
				mejorD = d;
				mejorX = nx;
				mejorY = ny;
			}
		}
	}
	p->zx = mejorX;
	p->zy = mejorY;

	//Si ha caido encima de un jugador, lo mata
	for (k = 0; k < p->num; k++)
	{
		if (p->vivo[k] && p->x[k] == p->zx && p->y[k] == p->zy)
		{
			p->vivo[k] = 0;
			p->tMuerte[k] = time(NULL);
			EnviaAPartida(p, "15/El zombie ha atrapado a %s", p->nombre[k]);
		}
	}
}

void AvanzaTurno(Partida *p, MYSQL *conn)
{
	//Pasa el turno al siguiente jugador vivo. Si ya han movido todos, mueve el zombie y empieza otra ronda
	int k;
	for (k = p->turno + 1; k < p->num; k++)
	{
		if (p->vivo[k])
		{
			p->turno = k;
			EnviarEstado(p);
			return;
		}
	}

	//Han movido todos: turno del zombie
	MueveZombie(p);

	int vivos = ContarVivos(p);
	p->turno = 0;
	for (k = 0; k < p->num; k++)
	{
		if (p->vivo[k])
		{
			p->turno = k;
			break;
		}
	}
	EnviarEstado(p);	//Asi todos ven el tablero final

	if (vivos == 0)
		TerminaPartida(p, conn, 0);
	else if (RONDAS_PARA_GANAR > 0 && p->ronda >= RONDAS_PARA_GANAR)
		TerminaPartida(p, conn, 1);
	else
		p->ronda++;
}

void GuardaPartida(Partida *p, MYSQL *conn, int duracion, time_t ahora)
{
	//Guarda la partida y los resultados en la base de datos (asi funcionan las consultas 3, 4 y 5)
	char fecha[40];
	char consulta[500];
	strftime(fecha, sizeof(fecha), "%Y-%m-%d %H:%M", localtime(&ahora));
	sprintf(consulta, "INSERT INTO PartidaTable (CODE, Fecha, Duration, PistolaFound) VALUES ('%s','%s',%d,0)", p->codigo, fecha, duracion);
	if (mysql_query(conn, consulta) != 0)
	{
		printf("Error al guardar la partida: %u %s\n", mysql_errno(conn), mysql_error(conn));
		return;
	}
	int k;
	for (k = 0; k < p->num; k++)
	{
		char nombreEsc[60];
		mysql_real_escape_string(conn, nombreEsc, p->nombre[k], strlen(p->nombre[k]));
		int tiempo = (int)((p->vivo[k] ? ahora : p->tMuerte[k]) - p->tInicio);
		int puntos = tiempo;	//Puntos = segundos sobrevividos
		sprintf(consulta, "INSERT INTO Relation (PlayerID, PartidaCode, PointsPartida, TiempoSobrevivido, ZombieKilled) "
			"SELECT ID, '%s', %d, %d, 0 FROM PlayerTable WHERE Name = '%s'", p->codigo, puntos, tiempo, nombreEsc);
		mysql_query(conn, consulta);
		sprintf(consulta, "UPDATE PlayerTable SET GlobalPoints = GlobalPoints + %d WHERE Name = '%s'", puntos, nombreEsc);
		mysql_query(conn, consulta);
	}
}

void TerminaPartida(Partida *p, MYSQL *conn, int ganan)
{
	time_t ahora = time(NULL);
	GuardaPartida(p, conn, (int)(ahora - p->tInicio), ahora);

	char viejo[10];
	strcpy(viejo, p->codigo);
	NuevoCodigo(p->codigo);	//El lobby sigue, pero con codigo nuevo (el viejo ya esta en la base de datos)
	p->iniciada = 0;
	EnviaAPartida(p, "13/%s/%s/%s", viejo, ganan ? "Victoria" : "Derrota", p->codigo);
	LimpiaDesconectados(p);
}

void QuitaDePartida(MYSQL *conn, const char *nombre)
{
	//Un jugador se ha desconectado: lo sacamos de su partida
	int pos;
	int idx = BuscaPartidaDeJugador(nombre, &pos);
	if (idx == -1) return;
	Partida *p = &partidas[idx];

	if (p->iniciada)
	{
		int eraSuTurno = (p->turno == pos);
		p->socket[pos] = -1;
		if (p->vivo[pos])
		{
			p->vivo[pos] = 0;
			p->tMuerte[pos] = time(NULL);
		}
		EnviaAPartida(p, "15/%s se ha desconectado", nombre);
		if (ContarVivos(p) == 0)
			TerminaPartida(p, conn, 0);
		else if (eraSuTurno)
			AvanzaTurno(p, conn);
		else
			EnviarEstado(p);
	}
	else
	{
		EliminaMiembro(p, pos);
	}
}

/* ---------------------------------------------------------------- */
/* Atencion de peticiones                                           */
/* ---------------------------------------------------------------- */

int SeparaUsuarioContra(char *datos, char **usuario, char **con)
{
	//Divide "usuario-contraseña" por el primer guion. 1 si es valido, 0 si no
	char *guion = strchr(datos, '-');
	if (guion == NULL) return 0;
	*guion = '\0';
	*usuario = datos;
	*con = guion + 1;
	if (strlen(*usuario) < 1 || strlen(*usuario) > 19) return 0;
	if (strlen(*con) < 1 || strlen(*con) > 29) return 0;
	if (strchr(*usuario, '/') != NULL) return 0;
	return 1;
}

void Escapa(MYSQL *conn, const char *origen, char *destino)
{
	//Evita inyeccion SQL. destino debe medir al menos 2*strlen(origen)+1
	mysql_real_escape_string(conn, destino, origen, strlen(origen));
}

void ProcesarInterno(Datos *d, char *linea, char *usuario, int *terminar)
{
	MYSQL *conn = d->conn;
	int sock = d->sock_conn;

	if (linea[0] < '0' || linea[0] > '9')
	{
		Enviar(sock, "14/Mensaje no valido");
		return;
	}
	int codigo = atoi(linea);
	char *resto = strchr(linea, '/');
	if (resto != NULL) { *resto = '\0'; resto++; }
	else { resto = linea + strlen(linea); }

	printf("Peticion de %s -> Codigo: %d, Datos: %s\n", usuario[0] ? usuario : "(sin sesion)", codigo, resto);

	char *saveptr;

	if (codigo == 0)	//Desconexion
	{
		*terminar = 1;
	}
	else if (codigo == 1)	//Registro: usuario-contraseña
	{
		char *u, *c;
		if (!SeparaUsuarioContra(resto, &u, &c))
		{
			Enviar(sock, "1/NO/Usuario o contraseña no validos (usuario de 1 a 19 caracteres, sin barras)");
			return;
		}
		char uEsc[60], cEsc[100], consulta[300];
		Escapa(conn, u, uEsc);
		Escapa(conn, c, cEsc);

		sprintf(consulta, "SELECT PlayerTable.Name FROM PlayerTable WHERE PlayerTable.Name = '%s'", uEsc);
		if (mysql_query(conn, consulta) != 0)
		{
			Enviar(sock, "1/NO/Error al consultar: %u %s", mysql_errno(conn), mysql_error(conn));
			return;
		}
		MYSQL_RES *resultado = mysql_store_result(conn);
		int rows = (resultado == NULL) ? 0 : (int)mysql_num_rows(resultado);
		if (resultado != NULL) mysql_free_result(resultado);

		if (rows > 0)
		{
			Enviar(sock, "1/NO/Nombre de usuario ya en uso");
		}
		else
		{
			sprintf(consulta, "INSERT INTO PlayerTable (Name, Contra, GlobalPoints) VALUES ('%s','%s',0)", uEsc, cEsc);
			if (mysql_query(conn, consulta) != 0)
				Enviar(sock, "1/NO/Error al registrar: %u %s", mysql_errno(conn), mysql_error(conn));
			else
				Enviar(sock, "1/OK/Usuario registrado correctamente, ya puedes iniciar sesión");
		}
	}
	else if (codigo == 2)	//Inicio de sesion: usuario-contraseña
	{
		if (usuario[0] != '\0')
		{
			Enviar(sock, "2/NO/Ya has iniciado sesión como %s", usuario);
			return;
		}
		char *u, *c;
		if (!SeparaUsuarioContra(resto, &u, &c))
		{
			Enviar(sock, "2/NO/Usuario o contraseña no validos");
			return;
		}
		char uEsc[60], cEsc[100], consulta[300];
		Escapa(conn, u, uEsc);
		Escapa(conn, c, cEsc);
		sprintf(consulta, "SELECT PlayerTable.Name FROM PlayerTable WHERE PlayerTable.Name = '%s' AND PlayerTable.Contra = '%s'", uEsc, cEsc);
		if (mysql_query(conn, consulta) != 0)
		{
			Enviar(sock, "2/NO/Error en la base de datos: %u %s", mysql_errno(conn), mysql_error(conn));
			return;
		}
		MYSQL_RES *resultado = mysql_store_result(conn);
		int rows = (resultado == NULL) ? 0 : (int)mysql_num_rows(resultado);
		if (resultado != NULL) mysql_free_result(resultado);

		if (rows == 0)
		{
			Enviar(sock, "2/NO/Usuario o contraseña incorrectos");
		}
		else if (DamePosicion(&miListaConectados, u) != -1)
		{
			Enviar(sock, "2/NO/Este usuario ya está conectado desde otro cliente");
		}
		else if (PonConectado(&miListaConectados, u, sock) == -1)
		{
			Enviar(sock, "2/NO/El servidor está lleno");
		}
		else
		{
			strcpy(usuario, u);
			Enviar(sock, "2/Correcto/Inicio de sesión correcto");
			NotificaConectados();
		}
	}
	else if (codigo == 3)	//Consulta: ¿el jugador encontro pistola?
	{
		char param[31], esc[70], consulta[500];
		strncpy(param, resto, 30); param[30] = '\0';
		Escapa(conn, param, esc);
		sprintf(consulta,
			"SELECT PartidaTable.Code "
			"FROM PlayerTable, PartidaTable, Relation "
			"WHERE PlayerTable.Name = '%s' "
			"AND PlayerTable.ID = Relation.PlayerID "
			"AND Relation.PartidaCode = PartidaTable.Code "
			"AND Relation.ZombieKilled = 1 "
			"AND PartidaTable.PistolaFound = 1", esc);
		if (mysql_query(conn, consulta) != 0)
		{
			Enviar(sock, "3/ERROR/Error al consultar: %u %s", mysql_errno(conn), mysql_error(conn));
			return;
		}
		MYSQL_RES *resultado = mysql_store_result(conn);
		if (resultado == NULL)
		{
			Enviar(sock, "3/ERROR/La consulta no ha devuelto resultado");
			return;
		}
		if (mysql_num_rows(resultado) == 0)
			Enviar(sock, "3/NO/El jugador %s no ha encontrado o usado pistola.", param);
		else
			Enviar(sock, "3/SI/Pistola encontrada.");
		mysql_free_result(resultado);
	}
	else if (codigo == 4)	//Consulta: ¿cuantos jugadores hay en la partida?
	{
		char param[31], esc[70], consulta[300];
		strncpy(param, resto, 30); param[30] = '\0';
		Escapa(conn, param, esc);
		sprintf(consulta, "SELECT COUNT(*) FROM Relation WHERE PartidaCode = '%s'", esc);
		if (mysql_query(conn, consulta) != 0)
		{
			Enviar(sock, "4/Error al consultar: %u %s", mysql_errno(conn), mysql_error(conn));
			return;
		}
		MYSQL_RES *resultado = mysql_store_result(conn);
		if (resultado == NULL)
		{
			Enviar(sock, "4/0");
			return;
		}
		MYSQL_ROW row = mysql_fetch_row(resultado);
		Enviar(sock, "4/%s", row != NULL ? row[0] : "0");
		mysql_free_result(resultado);
	}
	else if (codigo == 5)	//Consulta: ¿que jugador vivio mas tiempo en la partida?
	{
		char param[31], esc[70], consulta[400];
		strncpy(param, resto, 30); param[30] = '\0';
		Escapa(conn, param, esc);
		sprintf(consulta,
			"SELECT PlayerTable.Name, Relation.TiempoSobrevivido "
			"FROM PlayerTable, Relation "
			"WHERE PlayerTable.ID = Relation.PlayerID "
			"AND Relation.PartidaCode = '%s' "
			"ORDER BY Relation.TiempoSobrevivido DESC LIMIT 1;", esc);
		if (mysql_query(conn, consulta) != 0)
		{
			Enviar(sock, "5/Error al consultar: %u %s", mysql_errno(conn), mysql_error(conn));
			return;
		}
		MYSQL_RES *resultado = mysql_store_result(conn);
		if (resultado == NULL)
		{
			Enviar(sock, "5/No existe la partida %s", param);
			return;
		}
		MYSQL_ROW row = mysql_fetch_row(resultado);
		if (row == NULL)
			Enviar(sock, "5/No existe la partida %s o no tiene jugadores registrados", param);
		else
			Enviar(sock, "5/%s-%s", row[0], row[1]);
		mysql_free_result(resultado);
	}
	else if (codigo == 7)	//Invitar: 7/invitado
	{
		if (usuario[0] == '\0') { Enviar(sock, "14/Inicia sesión antes de invitar a alguien"); return; }
		if (strcmp(resto, usuario) == 0) { Enviar(sock, "14/No puedes invitarte a ti mismo"); return; }

		int posInv = DamePosicion(&miListaConectados, resto);
		if (posInv == -1) { Enviar(sock, "14/El jugador %s no está conectado", resto); return; }
		if (miListaConectados.conectados[posInv].invitacion[0] != '\0')
		{ Enviar(sock, "14/%s ya tiene una invitación pendiente", resto); return; }

		//¿El invitado ya esta en una partida? (si es un lobby de 1 persona, no cuenta)
		int posJ;
		int idxInv = BuscaPartidaDeJugador(resto, &posJ);
		if (idxInv != -1 && (partidas[idxInv].num > 1 || partidas[idxInv].iniciada))
		{ Enviar(sock, "14/%s ya está en otra partida", resto); return; }

		//Mi partida: si no tengo, la creamos
		int idx = BuscaPartidaDeJugador(usuario, &posJ);
		if (idx == -1) idx = CreaPartida(usuario, sock);
		if (idx == -1) { Enviar(sock, "14/El servidor no admite más partidas ahora mismo"); return; }
		Partida *p = &partidas[idx];
		if (p->iniciada) { Enviar(sock, "14/No puedes invitar durante una partida"); return; }
		if (p->num >= MAX_JUGADORES) { Enviar(sock, "14/La partida está llena (máximo %d jugadores)", MAX_JUGADORES); return; }

		strcpy(miListaConectados.conectados[posInv].invitacion, p->codigo);
		Enviar(miListaConectados.conectados[posInv].socket, "8/%s/%s", p->codigo, usuario);
		Enviar(sock, "15/Invitación enviada a %s", resto);
	}
	else if (codigo == 9)	//Respuesta a invitacion: 9/codigo/Aceptado|Rechazado
	{
		char *cod = strtok_r(resto, "/", &saveptr);
		char *res = strtok_r(NULL, "/", &saveptr);
		int pos = DamePosicion(&miListaConectados, usuario);
		if (pos == -1 || cod == NULL || res == NULL || strcmp(miListaConectados.conectados[pos].invitacion, cod) != 0)
		{
			Enviar(sock, "14/No tienes ninguna invitación pendiente");
			return;
		}
		miListaConectados.conectados[pos].invitacion[0] = '\0';

		int idx = BuscaPartidaPorCodigo(cod);
		if (idx == -1 || partidas[idx].iniciada || partidas[idx].num >= MAX_JUGADORES)
		{
			Enviar(sock, "14/La partida ya no está disponible");
			return;
		}
		Partida *p = &partidas[idx];

		if (strcmp(res, "Aceptado") == 0)
		{
			//Si yo tenia mi propio lobby vacio, lo disuelvo
			int posJ;
			int mio = BuscaPartidaDeJugador(usuario, &posJ);
			if (mio != -1)
			{
				if (partidas[mio].num == 1 && !partidas[mio].iniciada)
					partidas[mio].activa = 0;
				else
				{
					Enviar(sock, "14/Ya estás en otra partida");
					return;
				}
			}
			strcpy(p->nombre[p->num], usuario);
			p->socket[p->num] = sock;
			p->vivo[p->num] = 0;
			p->num++;

			//Todos reciben la lista completa del lobby (el anfitrion va primero)
			int k;
			for (k = 0; k < p->num; k++)
				EnviaAPartida(p, "7/%s/%s/Aceptado", p->codigo, p->nombre[k]);
		}
		else
		{
			Enviar(p->socket[0], "7/%s/%s/Rechazado", p->codigo, usuario);
		}
	}
	else if (codigo == 10)	//Iniciar partida: 10/codigo
	{
		int idx = BuscaPartidaPorCodigo(resto);
		if (usuario[0] == '\0' || idx == -1) { Enviar(sock, "14/No existe esa partida"); return; }
		Partida *p = &partidas[idx];
		if (strcmp(p->nombre[0], usuario) != 0) { Enviar(sock, "14/Solo el anfitrión (%s) puede iniciar la partida", p->nombre[0]); return; }
		if (p->iniciada) { Enviar(sock, "14/La partida ya está en marcha"); return; }
		if (p->num < MIN_JUGADORES) { Enviar(sock, "14/Hacen falta al menos %d jugadores para empezar", MIN_JUGADORES); return; }

		IniciaPartida(p);
		EnviaAPartida(p, "15/¡Empieza la partida! Sobrevivid %d rondas", RONDAS_PARA_GANAR);
		EnviarEstado(p);
	}
	else if (codigo == 12)	//Mover: 12/codigo/x/y
	{
		char *cod = strtok_r(resto, "/", &saveptr);
		char *sx = strtok_r(NULL, "/", &saveptr);
		char *sy = strtok_r(NULL, "/", &saveptr);
		if (usuario[0] == '\0' || cod == NULL || sx == NULL || sy == NULL)
		{ Enviar(sock, "14/Movimiento no valido"); return; }
		int idx = BuscaPartidaPorCodigo(cod);
		if (idx == -1 || !partidas[idx].iniciada) { Enviar(sock, "14/No hay ninguna partida en marcha"); return; }
		Partida *p = &partidas[idx];
		int k = p->turno;
		if (strcmp(p->nombre[k], usuario) != 0) { Enviar(sock, "14/No es tu turno, juega %s", p->nombre[k]); return; }

		int nx = atoi(sx);
		int ny = atoi(sy);
		if (nx < 0 || nx >= COLUMNAS || ny < 0 || ny >= FILAS) { Enviar(sock, "14/Esa casilla está fuera del tablero"); return; }
		if (Distancia(p->x[k], p->y[k], nx, ny) != 1) { Enviar(sock, "14/Solo puedes moverte a una casilla adyacente (arriba, abajo, izquierda o derecha)"); return; }
		if (Ocupada(p, nx, ny)) { Enviar(sock, "14/Esa casilla está ocupada"); return; }

		p->x[k] = nx;
		p->y[k] = ny;
		AvanzaTurno(p, conn);
	}
	else if (codigo == 16)	//Chat del lobby: 16/texto
	{
		int posJ;
		int idx = BuscaPartidaDeJugador(usuario, &posJ);
		if (usuario[0] == '\0' || idx == -1) { Enviar(sock, "14/Únete a una partida para usar el chat"); return; }
		if (strlen(resto) > 300) resto[300] = '\0';
		EnviaAPartida(&partidas[idx], "16/%s/%s", usuario, resto);
	}
	else
	{
		Enviar(sock, "14/Peticion desconocida (codigo %d)", codigo);
	}
}

void *AtenderCliente(void *arguments)
{
	Datos *d = (Datos *)arguments;
	int sock_conn = d->sock_conn;
	char usuario[20] = "";		//Nombre del usuario de esta conexion (vacio hasta que inicie sesion)

	char acumulado[4096];		//Los mensajes pueden llegar partidos o pegados: los separamos por '\n'
	int len = 0;
	char buffer[512];
	int terminar = 0;

	// Entramos en un bucle para atender todas las peticiones de este cliente hasta que se desconecte
	while (!terminar)
	{
		int ret = read(sock_conn, buffer, sizeof(buffer));
		if (ret <= 0) break;	//El cliente se ha ido (cerrado o caido)

		if (len + ret >= (int)sizeof(acumulado))
		{
			len = 0;	//Mensaje demasiado largo: lo descartamos
			continue;
		}
		memcpy(acumulado + len, buffer, ret);
		len += ret;

		int inicio = 0;
		int k;
		for (k = 0; k < len && !terminar; k++)
		{
			if (acumulado[k] == '\n')
			{
				acumulado[k] = '\0';
				char *linea = acumulado + inicio;
				int l = strlen(linea);
				if (l > 0 && linea[l - 1] == '\r') linea[l - 1] = '\0';
				if (linea[0] != '\0')
				{
					pthread_mutex_lock(&mutex);
					ProcesarInterno(d, linea, usuario, &terminar);
					pthread_mutex_unlock(&mutex);
				}
				inicio = k + 1;
			}
		}
		if (!terminar)
		{
			memmove(acumulado, acumulado + inicio, len - inicio);
			len -= inicio;
		}
	}

	// Limpieza: sacamos al jugador de su partida y de la lista de conectados
	pthread_mutex_lock(&mutex);
	if (usuario[0] != '\0')
	{
		QuitaDePartida(d->conn, usuario);
		if (EliminaConectado(&miListaConectados, usuario) == 0)
			printf("Eliminado %s\n", usuario);
		NotificaConectados();
	}
	pthread_mutex_unlock(&mutex);

	close(sock_conn);
	free(d);
	return NULL;
}

int main(int argc, char *argv[])
{
	signal(SIGPIPE, SIG_IGN);	//Si un cliente se cae, que no muera el servidor al escribirle
	srand(time(NULL));
	contadorCodigo = time(NULL) % 1000000;
	memset(partidas, 0, sizeof(partidas));

	/*MySQL*/

	MYSQL *conn;

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
		printf("Error al iniciar la conexion con MySQL (¿esta arrancado? ¿existe la base Zombie? ¿la contraseña es 'mysql'?)\n");
		exit(1);
	}

	/*Conexion servidor*/

	int sock_conn, sock_listen;
	struct sockaddr_in serv_adr;

	// INICIALIZACIONES
	// Abrimos el socket
	if ((sock_listen = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		printf("Error creant socket\n");
		exit(1);
	}
	int si = 1;
	setsockopt(sock_listen, SOL_SOCKET, SO_REUSEADDR, &si, sizeof(si));	//Para poder reiniciar el servidor sin esperar

	// Hacemos el bind al port
	memset(&serv_adr, 0, sizeof(serv_adr));// inicialitza a zero serv_addr
	serv_adr.sin_family = AF_INET;

	// asocia el socket a cualquiera de las IP de la maquina.
	//htonl formatea el numero que recibe al formato necesario
	serv_adr.sin_addr.s_addr = htonl(INADDR_ANY);
	// establecemos el puerto de escucha
	serv_adr.sin_port = htons(PUERTO);
	if (bind(sock_listen, (struct sockaddr *)&serv_adr, sizeof(serv_adr)) < 0)
	{
		printf("Error al bind (¿hay otro servidor ya abierto en el puerto %d?)\n", PUERTO);
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
		if (sock_conn < 0) continue;
		printf("He recibido conexion\n");

		//sock_conn es el socket que usaremos para este cliente y conn es la conexion a la BBDD
		Datos *d = malloc(sizeof(Datos));
		d->sock_conn = sock_conn;
		d->conn = conn;

		// Crear thread y decirle lo que tiene que hacer
		pthread_t thread;
		pthread_create(&thread, NULL, AtenderCliente, d);
		pthread_detach(thread);
	}

	mysql_close(conn);		//Cerrar conexion
	return 0;
}
