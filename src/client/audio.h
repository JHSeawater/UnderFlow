#ifndef AUDIO_H
#define AUDIO_H

// 오디오 백그라운드 엔진을 초기화하고 플레이리스트를 빌드하여 재생을 개시합니다.
// bgm_dir_path: BGM mp3 파일들이 들어있는 폴더 경로
int audio_init(const char *bgm_dir_path);

// 오디오 백그라운드 재생 스레드를 안전하게 일시정지하거나 종료 수거합니다.
void audio_shutdown(void);

// 특정 위기 상황에 삑 소리와 함께 화면을 번쩍이게 만드는 효과를 연출합니다.
void audio_play_alarm(void);

#endif // AUDIO_H
