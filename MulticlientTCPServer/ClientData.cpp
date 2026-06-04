#include <string>

class ClientData{
public:
    ClientData(int fd,const std::string& _name) : fd(fd), name(_name) {}

    void PrintInfo() const{
        const std::string banString = (bIsBanned) ? "banned" : "not banned";
        printf("Name: %s , FD: %d , Ban Status: %s\n",name.c_str(),fd,banString.c_str());
    };

    // Getters

    int get_fd() const { return fd; }

    std::string get_name() const { return name; }

    bool GetIsBanned() const {return bIsBanned;}

    // Setters

    bool SetIsBanned(bool bBanned){
        if(bIsBanned == true)
        {
            printf("%s is allready banned, cannot ban twice\n",name.c_str());
            return false;
        }

        bIsBanned = bBanned;
        return bIsBanned;
    }

private:
    int fd;
    std::string name;
    bool bIsBanned{false};
};