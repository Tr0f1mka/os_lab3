#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <time.h>

// Глобальные параметры задачи
int **input_matrix = NULL;
int **output_matrix = NULL;
int rows = 0, cols = 0;
int window_size = 3;
int K = 1;
int max_threads = 2;

sem_t thread_semaphore;

// Структура для передачи параметров в поток
typedef struct {
    int thread_id;
    int start_row;
    int end_row;
    int **src;
    int **dst;
} ThreadArg;


static double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}


// Сравнение для qsort
int cmp_int(const void *a, const void *b) {
    return (*(int*)a - *(int*)b);
}

// Медианный фильтр для одного элемента
int compute_median(int **mat, int r, int c, int w) {
    int half = w / 2;
    int count = w * w;
    int *window = (int*)malloc(count * sizeof(int));
	if (window == NULL) {
		perror("malloc");
		return 0;
	}
    int idx = 0;

    for (int i = -half; i <= half; i++) {
        for (int j = -half; j <= half; j++) {
            int nr = r + i;
            int nc = c + j;
            if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) {
                window[idx++] = 0;
            } else {
                window[idx++] = mat[nr][nc];
            }
        }
    }

    qsort(window, count, sizeof(int), cmp_int);
    int median = window[count / 2];
    free(window);
    return median;
}

// Функция потока: обрабатывает назначенные строки
void *worker(void *arg) {
	if (arg == NULL) {
		sem_post(&thread_semaphore);
		return NULL;
	}
    ThreadArg *targ = (ThreadArg*)arg;

    for (int r = targ->start_row; r < targ->end_row; r++) {
        for (int c = 0; c < cols; c++) {
            targ->dst[r][c] = compute_median(targ->src, r, c, window_size);
        }
    }

    sem_post(&thread_semaphore);

    free(targ);
    return NULL;
}

// Выделение матрицы
int **alloc_matrix(int r, int c) {
    int **mat = (int**)malloc(r * sizeof(int*));
    if (mat == NULL) {
		perror("malloc");
		return NULL;
	}
	for (int i = 0; i < r; i++) {
        mat[i] = (int*)malloc(c * sizeof(int));
		if (mat[i] == NULL) {
			perror("malloc");
			for (int j = 0; j < i; j++) free(mat[j]);
			free(mat);
			return NULL;
		}
    }
    return mat;
}

// Освобождение матрицы
void free_matrix(int **mat, int r) {
    for (int i = 0; i < r; i++) free(mat[i]);
    free(mat);
}

// Заполнение матрицы случайными числами
void fill_random(int **mat, int r, int c) {
	if (mat == NULL) {
		return;
	}
    for (int i = 0; i < r; i++) {
		for (int j = 0; j < c; j++) {
            mat[i][j] = rand() % 100;
		}
	}
}

// Вывод матрицы
void print_matrix(int **mat, int r, int c, const char *name) {
	if (mat == NULL) {
		printf("Пустая матрица\n");
		return;
	}
    printf("%s:\n", name);
    for (int i = 0; i < r; i++) {
        for (int j = 0; j < c; j++) printf("%4d ", mat[i][j]);
        printf("\n");
    }
    printf("\n");
}

int main(int argc, char *argv[]) {
    if (argc < 6) {
        fprintf(stderr, "Неправильные аргументы\n");
        return 1;
    }

    rows = atoi(argv[1]);
    cols = atoi(argv[2]);
    window_size = atoi(argv[3]);
    K = atoi(argv[4]);
    max_threads = atoi(argv[5]);

    if (window_size % 2 == 0 || window_size < 1) {
        fprintf(stderr, "Размер окна должен быть нечётным и больше нуля\n");
        return 1;
    }
    if (max_threads < 1) max_threads = 1;

    printf("Параметры: %d x %d, окно %d, K=%d, макс. потоков=%d\n",
           rows, cols, window_size, K, max_threads);

    srand(time(NULL));
	double t_global_start = now_sec();
	
	input_matrix = alloc_matrix(rows, cols);
	if (input_matrix == NULL) {
		return 1;
	}
    output_matrix = alloc_matrix(rows, cols);
	if (output_matrix == NULL) {
		return 1;
	}
    fill_random(input_matrix, rows, cols);
    // print_matrix(input_matrix, rows, cols, "Исходная матрица");

    sem_init(&thread_semaphore, 0, max_threads);

    int **src = input_matrix;
    int **dst = output_matrix;

	double t_start = now_sec();

    for (int iter = 0; iter < K; iter++) {
        int total_threads = (rows < max_threads) ? rows : max_threads;
        int rows_per_thread = (rows + total_threads - 1) / total_threads;
        pthread_t *threads = (pthread_t*)malloc(total_threads * sizeof(pthread_t));
		if (threads == NULL) {
			perror("malloc");
			break;
		}

        for (int t = 0; t < total_threads; t++) {
            sem_wait(&thread_semaphore);

            ThreadArg *arg = (ThreadArg*)malloc(sizeof(ThreadArg));
			if (arg == NULL) {
				for (int t = 0; t < total_threads; t++) {
					pthread_join(threads[t], NULL);
				}
				free(threads);
				sem_destroy(&thread_semaphore);
				free_matrix(input_matrix, rows);
				free_matrix(output_matrix, rows);
				return 1;
			}
            arg->thread_id = t;
            arg->start_row = t * rows_per_thread;
            arg->end_row = (t + 1) * rows_per_thread;
            if (arg->end_row > rows) arg->end_row = rows;
            arg->src = src;
            arg->dst = dst;

            if (pthread_create(&threads[t], NULL, worker, arg) != 0) {
				for (int k = 0; k < total_threads; k++) {
					pthread_join(threads[k], NULL);
				}
				free(threads);
				free_matrix(input_matrix, rows);
				free_matrix(output_matrix, rows);
				free(arg);
				sem_destroy(&thread_semaphore);
                perror("pthread_create");
				return 1;
            }
        }

        for (int t = 0; t < total_threads; t++) {
            pthread_join(threads[t], NULL);
        }
        free(threads);

        int **tmp = src;
        src = dst;
        dst = tmp;
    }

	double t_end = now_sec();

    // print_matrix(src, rows, cols, "Результат после K итераций");
    sem_destroy(&thread_semaphore);
    free_matrix(input_matrix, rows);
    free_matrix(output_matrix, rows);

	double t_global_end = now_sec();

	printf("Потрачено времени: %.6f\n", t_end - t_start);
	printf("Потрачено всего времени: %.6f\n", t_global_end - t_global_start);
	
    return 0;
}