#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <curl/curl.h>

int cantHilos;
long long int tiempo;
int termino;
FILE * sitios;
FILE * config;
sem_t semaforoSitio;
sem_t semaforoVisitados;
sem_t semaforoSpider;

// Prototipo de la funcion estaVisitado
bool estaVisitado(char[512]);

// Estructura auxiliar para guardar la cadena de texto con el html
//  y el tamaño que esta tiene
typedef struct mem
{
    char *memory;
    size_t size;
} mem;

// Función que es llamada por curl cada vez que recibe un bloque de datos desde la url
size_t write_callback(void *contenido, size_t size, size_t nmemb, void *userp)
{
    // Calculamos el tamaño del bloque
    size_t tamanioreal = size * nmemb;
    // Recuperamos el puntero a la memoria donde dijimos que íbamos a dejar todo
    mem *memoria = (mem *)userp;

    // Intentamos extender el tamaño de la memoria al tamaño actual + tamaño del bloque nuevo que entra
    char *ptr = realloc(memoria->memory, memoria->size + tamanioreal + 1);

    // Si retorna null, entonces no hay memoria y esto falló
    if (!ptr)
    {
        printf("Sin Memoria!");
        return 0;
    }

    // Si hay memoria, re ajustamos los punteros y copiamos el contenido del nuevo
    //   bloque al final del bloque anterior
    memoria->memory = ptr;
    memcpy(&(memoria->memory[memoria->size]), contenido, tamanioreal);
    memoria->size += tamanioreal;
    memoria->memory[memoria->size] = 0;

    // Retornamos el tamaño del bloque recibido
    return tamanioreal;
}

// Función que utiliza curl.h para extraer el html de una página y guardarlo en memoria
mem *fetch_url(char *url, CURL* curl)
{

    // Inicializaciones básicas
    CURLcode res;
    mem *memoria = (mem *)malloc(sizeof(mem));
    // Esto se irá ajustando de acuerdo a cuánto necesite
    memoria->memory = malloc(1);
    memoria->size = 0;

    if (!curl)
    {
        printf("No se pudo inicializar :c \n");
        return memoria;
    }

    sem_wait(&semaforoSpider);

    // Se inicializa la petición de curl con la url a pedir
    curl_easy_setopt(curl, CURLOPT_URL, url);

    curl_easy_setopt( curl, CURLOPT_NOSIGNAL, 0 );

    // Se establece que cada vez que se recibe un bloque de datos desde la url,
    //  se llama a la función write_callback
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    // El contenido lo escribiermos sobre la variable memoria
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)memoria);

    // Algunas páginas exigen que se identifiquen, decimos que estamos usando curl
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");

    // Ejecutamos la petición
    res = curl_easy_perform(curl);

    sem_post(&semaforoSpider);

    // Si la petición falla, imprimimos el error.
    if (res != CURLE_OK)
    {
        fprintf(stderr, "curl_easy_perform() falla: %s\n", curl_easy_strerror(res));
    }

    // Retornamos el contenido html
    return memoria;
}

// Araña de un webcrawler.
//  Esta sólo extrae los enlaces y los imprime.
void spider(void *data, CURL* curl)
{

    char *url = (char *)data;
    // Si la URL esta vacia, entonces no hacemos nada.
    if(strcmp(url, "") == 0){
        return;
    }
    // Extrae todo el html de la url
    mem *memoria = fetch_url(url, curl);
    // Comienza buscando el primer enlace (Asumiendo que está en una propiedad href)
    char *inicio = strstr(memoria->memory, "href=\"");

    if(inicio != NULL){

        char *final = NULL;
        int size;
        char *aux;
        char *fullURL;

        // Se va recorriendo cada propiedad href de la página
        //  y se le imprime
        do
        {
            // para quitar  ' href=" ' del string
            inicio += 6;
            // Se busca desde el inicio hasta el siguiente ", -1 para que no lo contenga
            final = strstr(inicio, "\"") - 1;
            
            // +2 por el \0 y el espacio extra.
            size = final - inicio + 2;
            aux = (char *)malloc(sizeof(char) * size + 1);
            fullURL = (char *)malloc(sizeof(char) * size + (strlen(url)));

            strncpy(aux, inicio, size + 1);
            // Se coloca el caracter nulo
            aux[size - 1] = '\0';

            // Cuando se enlaza dentro del mismo dominio, es costumbre no colocar la url completa
            // para asegurarnos que no se recorra el mismo enlace más de una vez, debe comenzar con su dominio
            if (aux[0] == '/')
            {
                // Primero debemos remover el slash del aux
                memmove(aux, aux+1, size);
                // Para ahorrar lineas de codigo, guardamos la url completa en la variable fullURL
                // de esta manera, solo hacemos el llamado a 1 printf y fprintf
                strcpy(fullURL, url);
                strcat(fullURL, aux);
            }
            else
            {
                strcpy(fullURL, aux);
            }

            // Comprobamos tambien si es que ya visitamos el sitio en el archivo visitados,
            // si es que ya esta visitado, solo mostramos un mensaje por pantalla
            if(estaVisitado(fullURL)){
                // Por ahora el mensaje esta comentado para evitar mucho texto en la consola
                // printf("Sitio visitado\n");
            }
            else{
                sem_wait(&semaforoVisitados);

                FILE * visitados = fopen("visitados.txt", "a");
                fprintf(visitados, "%s\n", fullURL);
                printf("visitando -> %s\n\n", fullURL);
                // printf("%s\n\n", fullURL);
                fclose(visitados);

                sem_post(&semaforoVisitados);
            }

            // Se libera la memoria porque un webcrawler puede requerir demasiados recursos
            free(aux);
            free(fullURL);
            aux = NULL;
            fullURL = NULL;

        // Busca el siguiente enlace
        } while ((inicio = strstr(inicio, "href=\"")) != NULL);
    }
    
}

// Se encargara de dar termino al programa, la condicion de termino cambia cuando el sleep termine
void * threadTiempo(void * arg){
    long long int tiempo = (long long int)arg;
    termino = 0;
    sleep(tiempo);
    termino = 1;
}

void * threadSpider(void * arg){

    // Mientras nuestro thread tiempo no llegue al tiempo indicado, se seguirá ejectutando los spiders
    while(!termino){

        CURL * curl = (CURL*) arg;

        // Obtenemos un sitio mas desde el archivo de sitios
        sem_wait(&semaforoSitio);
        char sitioPorVisitar[128];
        fgets(sitioPorVisitar, 128, sitios);
        sem_post(&semaforoSitio);

        // Si es que sitioPorVisitar es vacio no debemos leerlo
        if(sitioPorVisitar != NULL && strcmp(sitioPorVisitar, "") != 0){

            // Solo realizamos la operacion si es que el sitio no estaba visitado
            if(!estaVisitado(sitioPorVisitar)){
                sitioPorVisitar[strcspn(sitioPorVisitar, "\n")] = '\0';
                spider(sitioPorVisitar, curl);
            }

        }
        else{
            // Si llegamos al final del archivo, entonces debemos volver al principio
            sem_wait(&semaforoSitio);
            rewind(sitios);
            sem_post(&semaforoSitio);
        }

    }
    
}

bool estaVisitado(char sitioPorVisitar[512]){

    // Nos aseguramos de que el sitio ingresado no contenga salto de linea
    sitioPorVisitar[strcspn(sitioPorVisitar, "\n")] = '\0';

    sem_wait(&semaforoVisitados);

    // Se puede dar el caso de que visitados.txt no exista aun, por lo que debemos asegurarnos
    // de que no sea NULL.
    FILE * visitadosFile = fopen("visitados.txt", "r");
    if(visitadosFile == NULL){
        sem_post(&semaforoVisitados);
        return false;
    }

    char sitioVisitado[512];

    while(fgets(sitioVisitado, 512, visitadosFile) != NULL){
        // Nos aseguramos de que el sitio tomado del archivo no contenga salto de linea
        sitioVisitado[strcspn(sitioVisitado, "\n")] = '\0';
        // Si es que el archivo visitados tenia el archivo por visitar, entonces retornamos true
        if(strcmp(sitioPorVisitar, sitioVisitado) == 0){
            fclose(visitadosFile);
            sem_post(&semaforoVisitados);
            return true;
        }
    }

    fclose(visitadosFile);
    sem_post(&semaforoVisitados);
    return false;
}

void initConfig(){
    char linea[16];

    config = fopen("config.txt", "r");

    // Leemos la cantidad de hilos desde el archivo config.txt
    fgets(linea, 16, config); 
    cantHilos = atoi(linea);

    // Leemos la cantidad de tiempo desde el archivo config.txt
    fgets(linea, 16, config);
    tiempo = atoi(linea);

    fclose(config);
}

void start (){

    // Inicializamos la configuración
    initConfig();

    // Inicializamos los semaforos
    sem_init(&semaforoSitio, 0, 1);
    sem_init(&semaforoVisitados, 0, 1);
    sem_init(&semaforoSpider, 0, 1);

    // Abrimos el archivo para borrar su contenido antes de iniciiar el algoritmo
    FILE * visitados = fopen("visitados.txt", "w");
    fclose(visitados);

    // Abrimos el archivo de sitios en modo lectura
    sitios = fopen("sitios.txt", "r");

}

int main(int argc, char *argv)
{
    // Llamamamos a la funcion start que inicializa la configuracion y otras variables globales
    start();

    // Inicializamos la librería curl y una instancia para manejar las solicitudes
    curl_global_init(CURL_GLOBAL_ALL);
    CURL* curl = curl_easy_init();

    // Creamos un hilo de tiempo, este hilo se ejecutará paralelamente con los scripts,
    // cuando pase el tiempo que se pide (en segundos), el programa se terminará
    pthread_t hiloTiempo;

    // Creamos un array de hilos, que serán los procesos que se ejecutarán en paralelo
    pthread_t hilos[cantHilos];

    // Creamos todos los hilos con pthread_create
    pthread_create(&hiloTiempo, NULL, &threadTiempo, (void *)(long long int)tiempo);
    for(int i = 0 ; i < cantHilos ; i++){
        pthread_create(&hilos[i], NULL, &threadSpider, (void*)curl);
        printf("Creado hilo %d\n", i+1);
    }

    // Hacemos join de todos los hilos con pthread_join
    pthread_join(hiloTiempo, NULL);
    for(int i = 0 ; i < cantHilos ; i++){
        // printf("\nJoined hilo %d\n", i);
        pthread_join(hilos[i], NULL);
    }

    return 0;
}
