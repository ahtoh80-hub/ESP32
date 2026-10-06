git config --global user.name "Ваше Имя"
git config --global user.email "you@example.com"
git config --global core.editor "nano"
git config --list

git init                          # создать новый репозиторий в текущей папке
git clone <url>                   # клонировать удалённый репозиторий
git clone <url> my-folder         # клонировать в папку my-folder

git status                        # состояние файлов
git add file.txt                  # добавить файл в индекс
git add .                         # добавить все изменения
git commit -m "Сообщение"         # зафиксировать изменения
git commit -am "Сообщение"        # add + commit для уже отслеживаемых файлов

git branch                        # список локальных веток
git branch -a                     # список всех веток, включая удалённые
git branch new-branch             # создать ветку
git checkout new-branch           # переключиться на ветку
git switch new-branch             # современный вариант переключения
git checkout -b new-branch        # создать и переключиться
git switch -c new-branch          # современный вариант создать и переключиться
git merge feature                 # влить ветку feature в текущую
git branch -d feature             # удалить ветку после слияния
git branch -D feature             # удалить ветку принудительно

git remote -v                     # список удалённых репозиториев
git remote add origin <url>       # добавить удалённый репозиторий
git remote remove origin          # удалить удалённый репозиторий
git fetch                         # получить изменения, не сливая
git pull                          # получить и слить изменения
git push                          # отправить изменения
git push -u origin main           # отправить и запомнить upstream
git push origin --delete branch   # удалить удалённую ветку
